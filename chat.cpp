#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <Windows.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define CLIENT_ID_LIMIT			64
#define SERVER_FORMAT			"server"
#define CHAT_FILE_NAME			"messages.txt"
#define SIZE_BUFFER				512
#define MAX_CLIENTMSG_LEN		128
#define SERVER_MAILSLOT_PREFIX	TEXT("\\\\.\\mailslot\\")


// Структура описывает формат сообщения, передаваемого сервером клиенту
struct ms_srv_msg
{
	char client_id[CLIENT_ID_LIMIT + 1];
	char msg_data[MAX_CLIENTMSG_LEN + 1];
};


// Структуры, описывающие формат сообщений,
// передаваемых от клиентов
enum ms_msg_type
{
	// Тип сообщения
	ms_msg_type_connect = 0, // Подключение
	ms_msg_type_disconnect, // Отключение
	ms_msg_type_message, // Текстовое сообщение
	ms_msg_type_message_stop
};

struct ms_cli_msg
{
	char client_id[CLIENT_ID_LIMIT + 1];
	char msg_data[MAX_CLIENTMSG_LEN + 1];
	enum ms_msg_type msg_type;
};

struct client_ctx
{
	char* cli_id; // Идентификатор
	HANDLE cli_mslot; // Подключение к его mailslot
	struct client_ctx* next; // След. запись
};


void usage(char *);
void parseArguments(char **);
int startServer(char *);
int startClient(char *);
void logMessage(char *);

void mailslot_process_msg(struct ms_cli_msg*);
void client_add(const char*, char*);
void client_onmsg(const char*, const char*);
void clients_close_all(void);
void client_remove(const char*);
DWORD WINAPI stdin_thread(LPVOID);
void on_srv_msg(struct ms_srv_msg*);
static void clients_send_message(const char*, const char*);

FILE *messageFile = NULL;
char * nameMailslot = NULL;
HANDLE g_ev_stop;
DWORD id = 0u;
HANDLE send_thread;
struct client_ctx* g_clients;
HANDLE srv_mslot;
char message[CLIENT_ID_LIMIT + MAX_CLIENTMSG_LEN + 1];

char g_client_id[CLIENT_ID_LIMIT];
wchar_t server_name_mailslot[SIZE_BUFFER];
wchar_t mailslot_parameter[CLIENT_ID_LIMIT];
char g_cli_mslot_name[CLIENT_ID_LIMIT];


void
clear(void)
{
	while (getchar() != '\n');
}

int main(int argc, char *argv[])
{

	if (argc < 3 || argc > 4)
	{
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if ((messageFile = fopen(CHAT_FILE_NAME, "w")) == NULL) {
		printf("Error: can't open file %s", CHAT_FILE_NAME);
	}
	parseArguments(argv);
}


int startClient(char *a)
{
	OVERLAPPED overlap;
	HANDLE cli_mslot;


	// Подбор идентификатора клиента, а затем - cоздание mailslot для "обратной связи".
	// Имя этого mailslot будет передано серверу.
	// Сервер будет отправлять статистику через этот mailslot этому клиенту.
	// Если создать mailslot с определенным именем не удалось - значит он уже
	// был создан другим клиентом и надо увеличить наш номер.

	// Т.к. имя объекта сформировано в ANSI-строке, то вызывается ANSI-версия: CreateMailslotA.
	cli_mslot = CreateMailslotA(g_cli_mslot_name, sizeof(struct ms_srv_msg), MAILSLOT_WAIT_FOREVER, NULL);
	if (cli_mslot != INVALID_HANDLE_VALUE)
	{
		printf("My client_id: '%s'.\n", g_client_id);
	}
	if (!cli_mslot)
	{
		printf("Failed CreateMailslot. Error: %u.\n", GetLastError());
		return -1;
	}
	// Создание потока, производящего считывание сообщений из stdin.
	send_thread = CreateThread(NULL, id, &stdin_thread, cli_mslot, 0, NULL);
	// Создание overlap и события, которое будет переводиться в сигнальное состояние
	// при поступлении сообщений от сервера
	memset(&overlap, 0, sizeof(overlap));
	overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	while (TRUE)
	{
		struct ms_srv_msg srv_msg;
		DWORD read = 0;
		HANDLE wev[] = { overlap.hEvent, send_thread };
		BOOL ret;
		DWORD wr;
		ret = ReadFile(cli_mslot, &srv_msg, sizeof(srv_msg), &read, &overlap);
		if (ret == TRUE)
		{
			if (read != sizeof(srv_msg))
			{
				printf("ReadFile() error: %u bytes returned.\n", read);
				break;
			}
			// От сервера принято уведомление, вывод
			on_srv_msg(&srv_msg);
			continue;
		}
		if (GetLastError() != ERROR_IO_PENDING)
		{
			printf("ReadFile() unknown error: %u.\n", GetLastError());
			break;
		}
		wr = WaitForMultipleObjects(2, wev, FALSE, INFINITE);
		if (wr == WAIT_OBJECT_0)
		{
			// От сервера поступило сообщение
			if (!GetOverlappedResult(cli_mslot, &overlap, &read, FALSE))
			{
				printf("GetOverlappedResult returns error: %u.\n", GetLastError());
				break;
			}
			if (read != sizeof(srv_msg))
			{
				printf("ReadFile() returns incorrect size: %u.\n", read);
				break;
			}
			on_srv_msg(&srv_msg);
		}
		else if (wr == WAIT_OBJECT_0 + 1)
		{
			// Завершился поток считывания сообщений и отправки => нужно завершаться
			CancelIo(cli_mslot);
			WaitForSingleObject(overlap.hEvent, INFINITE);
			break;
		}
	}
	CloseHandle(send_thread);
	CloseHandle(overlap.hEvent);
	CloseHandle(cli_mslot);

	return EXIT_SUCCESS;
}


void on_srv_msg(struct ms_srv_msg* msg)
{
	if (!strcmp(msg->msg_data, "stop"))
	{
		TerminateThread(send_thread, EXIT_SUCCESS);
		return;
	}
	printf("%s %s\n", msg->client_id, msg->msg_data);
}

// Поточная функция, считывает в бесконечном цикле
// строки из stdin и завершается когда пользователь введет stop.
// Если пользователь введет что-то другое - то это сообщение отправляется
// на сервер с помощью mailslot (синхронно).
DWORD WINAPI stdin_thread(LPVOID param)
{
	char buf[128] = { 0 };
	HANDLE cli_mslot = (HANDLE)param;
	struct ms_cli_msg cli_msg;
	DWORD written;


	// Подключение к mailslot сервера
	srv_mslot = CreateFileW(server_name_mailslot, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (srv_mslot == INVALID_HANDLE_VALUE)
	{
		printf("Connect failed: %u.\n", GetLastError());
		return -1;
	}
	// Отправка уведомления о подключении, в т.ч. имя mailslot для обратной связи
	memset(&cli_msg, 0, sizeof(cli_msg));
	strcpy(cli_msg.client_id, g_client_id);
	strcpy(cli_msg.msg_data, g_cli_mslot_name);
	cli_msg.msg_type = ms_msg_type_connect;
	if (!WriteFile(srv_mslot, &cli_msg, sizeof(cli_msg), &written, NULL) || written !=
		sizeof(cli_msg))
	{
		printf("WriteFile() error: %u\n", GetLastError());
		CloseHandle(srv_mslot);
		return -2;
	}
	// Цикл считывания и передачи сообщений (пока не введут 'stop')
	while (TRUE)
	{
		fgets(buf, 128, stdin);
		while (strlen(buf) > 0 && buf[strlen(buf) - 1] == '\n')
			buf[strlen(buf) - 1] = 0;
		if (strlen(buf) == 127)
		{
			clear();
		}
		memset(&cli_msg, 0, sizeof(cli_msg));
		strcpy(cli_msg.client_id, g_client_id);
		if (strcmp(buf, "exit"))
		{

			if (!strcmp(buf, "stop")) {
				cli_msg.msg_type = ms_msg_type_message_stop;
			}
			else {
				cli_msg.msg_type = ms_msg_type_message;

			}
			strncpy(cli_msg.msg_data, buf, sizeof(cli_msg.msg_data) - 1);
		}
		else
		{
			cli_msg.msg_type = ms_msg_type_disconnect;
		}
		if (!WriteFile(srv_mslot, &cli_msg, sizeof(cli_msg), &written, NULL) || written !=
			sizeof(cli_msg))
		{
			printf("WriteFile() error: %u\n", GetLastError());
			break;
		}
		if (strcmp(buf, "exit"))
		{
			printf("Message send.\n");
			if (!strcmp(buf, "stop"))
			{
				printf("Terminating...\n");
				break;
			}
		}
		else
		{
			printf("Terminating...\n");
			break;
		}
	}
	// Отключение
	CloseHandle(srv_mslot);
	return 0;
}









// Функция удаляет информацию обо всех подключенных клиентах
void clients_close_all()
{
	while (g_clients)
		client_remove(g_clients->cli_id);
}

// Функция удаляет указанного клиента из списка, освобождает ресурсы
void client_remove(const char* cli_id)
{
	// Удалить указанного клиента
	struct client_ctx *cur = g_clients, *prev = NULL;
	while (cur)
	{
		if (!strcmp(cli_id, cur->cli_id))
		{
			// Клиент найден, удаление его из списка
			if (prev)
				prev->next = cur->next;
			else
				g_clients = cur->next;
			CloseHandle(cur->cli_mslot);
			printf("Client %s removed from list.\n", cur->cli_id);
			memset(message, 0x00, CLIENT_ID_LIMIT + MAX_CLIENTMSG_LEN + 1);
			sprintf(message, "%s disconnect", cli_id);
			logMessage(message);
			free(cur);
			return;
		}
		prev = cur;
		cur = cur->next;
	}
}

// Функция обрабатывает поступившее от клиента сообщение
void client_onmsg(const char* cli_id, const char* msg)
{
	memset(message, 0x00, CLIENT_ID_LIMIT + MAX_CLIENTMSG_LEN + 1);
	sprintf(message, "%s %s", cli_id, msg);
	logMessage(message);

	clients_send_message(cli_id, msg);
	if (!strcmp(msg, "stop"))
	{
		SetEvent(g_ev_stop);
	}
}


void client_add(const char* cli_id, char* mslot_name_buf)
{
	struct client_ctx* c;

	// Поиск клиента с таким идентификатором. Если он уже есть - то переоткрыть mailslot
	c = g_clients;
	while (c)
	{
		if (!strcmp(cli_id, c->cli_id))
		{
			// Переоткрытие его mailslot и выход
			printf("Client '%s' reconnected.\n", cli_id);
			memset(message, 0x00, CLIENT_ID_LIMIT + MAX_CLIENTMSG_LEN + 1);
			sprintf(message, "%s reconnected", cli_id);
			logMessage(message);
			CloseHandle(c->cli_mslot);
			c->cli_mslot = CreateFileA(mslot_name_buf, GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
			if (c->cli_mslot == INVALID_HANDLE_VALUE)
			{
				printf("Reconnect failed. CreateFile returns error: %u.\n",
					GetLastError());
			}
			return;
		}
		c = c->next;
	}
	c = (struct client_ctx*) malloc(sizeof(struct client_ctx));
	// Подключение к клиентскому mailslot'у. Имя было передано как ANSI-строка,
	// поэтому вызывается функция с суффиксом A.
	c->cli_mslot = CreateFileA(mslot_name_buf, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (c->cli_mslot == INVALID_HANDLE_VALUE)
	{
		// Подключиться не удалось
		printf("Failed CreateFile(): %u\n", GetLastError());
		free(c);
		return;
	}
	c->cli_id = (char*)malloc(strlen(cli_id) + 1);
	strcpy_s(c->cli_id, strlen(cli_id) + 1, cli_id);
	c->next = g_clients;
	g_clients = c;
	printf("Client '%s' connected.\n", c->cli_id);
	memset(message, 0x00, CLIENT_ID_LIMIT + MAX_CLIENTMSG_LEN + 1);
	sprintf(message, "%s connect", cli_id);
	logMessage(message);
}

// Функция обрабатывает принятые данные - сообщение от клиента
void mailslot_process_msg(struct ms_cli_msg* cli_msg)
{
	// "Обезопасить" стуктуру - записать терминальные нули в строковые поля
	cli_msg->client_id[sizeof(cli_msg->client_id) - 1] = 0;
	cli_msg->msg_data[sizeof(cli_msg->msg_data) - 1] = 0;
	if (cli_msg->msg_type == ms_msg_type_connect)
	{
		// Клиент подключился
		client_add(cli_msg->client_id, cli_msg->msg_data);
	}
	else if (cli_msg->msg_type == ms_msg_type_disconnect)
	{
		// Клиент отключился
		client_remove(cli_msg->client_id);
	}
	else if (cli_msg->msg_type == ms_msg_type_message)
	{
		// Клиент прислал сообщение
		client_onmsg(cli_msg->client_id, cli_msg->msg_data);
	}
	else if (cli_msg->msg_type == ms_msg_type_message_stop)
	{
		// Клиент прислал сообщение
		client_onmsg(cli_msg->client_id, cli_msg->msg_data);
	}
	else
	{
		printf("Unknown message type received: %u!\n", (ULONG)cli_msg->msg_type);
	}
}

int startServer(char *srv_mailslot_parameter)
{
	HANDLE srv_mslot;
	OVERLAPPED overlap;
	BOOL reading;

	// Создание серверного mailslot для принятия сообщений от клиентов
	srv_mslot = CreateMailslot(server_name_mailslot, sizeof(struct ms_cli_msg), MAILSLOT_WAIT_FOREVER, NULL);
	if (srv_mslot == INVALID_HANDLE_VALUE)
	{
		printf("CreateMailslot() failed: %u\n", GetLastError());
		return EXIT_FAILURE;
	}
	g_ev_stop = CreateEvent(NULL, TRUE, FALSE, NULL);

	// Ининциализация overlap: создание события, которое будет
	// "срабатывать" когда в mailslot придут данные от клиента
	memset(&overlap, 0, sizeof(overlap));
	overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	// Флаг, установлен в TRUE если происходит чтение
	reading = FALSE;

	while (TRUE)
	{
		struct ms_cli_msg cli_msg;
		DWORD read = 0;
		BOOL ret;
		HANDLE wev[] = { overlap.hEvent, g_ev_stop };
		DWORD wr;

		if (!reading)
		{
			// Если сейчас не происходит чтение - то начать его.
			ret = ReadFile(srv_mslot, &cli_msg, sizeof(cli_msg), &read, &overlap);
			if (ret)
			{
				// Данные от клиента пришли немедленно, обработка
				if (read == sizeof(cli_msg))
				{
					// Обработать сообщение
					mailslot_process_msg(&cli_msg);
				}
				else
				{
					printf("read = %u, error.\n", read);
				}
				reading = FALSE;
				continue;
			}
			// Проверка что ReadFile вернул корректную ошибку: ERROR_IO_PENDING
			if (GetLastError() != ERROR_IO_PENDING)
			{
				printf("ReadFile() returns error: %u.\n", GetLastError());
				reading = FALSE;
				continue;
			}
			// Чтение начато
			reading = TRUE;
		}
		// Ожидание одного из событий либо таймаута
		wr = WaitForMultipleObjects(2, wev, FALSE, 5000);
		if (wr == WAIT_OBJECT_0)
		{
			// Сработало событие из overlap => чтение завершено и поступило сообщение от клиента
			reading = FALSE;
			if (!GetOverlappedResult(srv_mslot, &overlap, &read, FALSE))
			{
				printf("GetOverlappedResult returns error: %u.\n", GetLastError());
				continue;
			}
			if (read != sizeof(cli_msg))
			{
				printf("GetOverlappedResult returns incorrect size: %u.\n", read);
				continue;
			}
			// Обработка сообщения
			mailslot_process_msg(&cli_msg);
		}
		else if (wr == WAIT_OBJECT_0 + 1)
		{
			// Сервер должен быть завершен, т.к. событие g_ev_stop
			// перешло в сигнальное состояние
			reading = FALSE;
			CancelIo(srv_mslot);
			WaitForSingleObject(overlap.hEvent, INFINITE);
			break;
		}
	}
	// Освободить ресурсы
	clients_close_all();
	CloseHandle(overlap.hEvent);
	CloseHandle(srv_mslot);
	CloseHandle(g_ev_stop);
	fclose(messageFile);
	return EXIT_SUCCESS;
}

// Функция рассылает статистику всем клиентам: каждому клиенту в его "персональный" mailslot
static void clients_send_message(const char* cli_id, const char* msg)
{
	struct client_ctx *cur = g_clients;
	DWORD written;
	struct ms_srv_msg srv_msg;
	strcpy(srv_msg.client_id, cli_id);
	strcpy(srv_msg.msg_data, msg);
	while (cur)
	{
		if (strcmp(cur->cli_id, cli_id))
		{
			if (!WriteFile(cur->cli_mslot, &srv_msg, sizeof(srv_msg), &written, NULL) || written !=
				sizeof(srv_msg))
			{
				printf("WriteFile() error: %u. Client: %s\n", GetLastError(), cur->cli_id);
			}
		}
		cur = cur->next;
	}
}

void parseArguments(char **argv)
{
	memset(server_name_mailslot, 0x00, sizeof(wchar_t) * SIZE_BUFFER);
	memset(mailslot_parameter, 0x00, sizeof(wchar_t) * CLIENT_ID_LIMIT);
	wcscpy(server_name_mailslot, SERVER_MAILSLOT_PREFIX);
	mbstowcs(mailslot_parameter, argv[2], strlen(argv[2]));
	wcscat(server_name_mailslot, mailslot_parameter);

	if (!strncmp(argv[1], SERVER_FORMAT, strlen(SERVER_FORMAT)))
	{
		startServer(argv[2]);
	}
	else {

		sprintf(g_cli_mslot_name, "\\\\.\\mailslot\\%s%s", argv[1], argv[2]);
		sprintf(g_client_id, "%s", argv[1]);
		startClient(argv[3]);
	}
}

void logMessage(char *message)
{
	time_t rawtime;
	struct tm * timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	fprintf(messageFile, "%d:%d:%d %s\n", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, message);
}

void usage(char *programName)
{
	fprintf(stdout, "Usage: %s server <name pipe> for server format \n"
		"%s <id client> <name pipe>\n", programName, programName);
}