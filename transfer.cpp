#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#define SERVER_PIPE_PREFIX		TEXT("\\\\.\\pipe\\")
#define CLIENT_ID_LIMIT			64
#define SERVER_FORMAT			"server"
#define TRANSFER_FILE_NAME		"transfers.txt"
#define SIZE_BUFFER				512

// Количество обработанных запросов от клиентов
CRITICAL_SECTION g_lock;
volatile int g_requests = 0;
// Событие будет в сигнальном состоянии при завершении программы
HANDLE g_event_stop = 0;
// Потоки сервера
HANDLE* g_threads = 0;
unsigned g_threads_cnt = 0;
unsigned g_threads_max_cnt = 0;
FILE *transferFile = NULL;
char message[512];

void logMessage(char *);
void usage(char *);
int startServer(void);
int startClient(char *);
void parseArguments(char **);
void server_stop(void);

wchar_t server_name_pipe[SIZE_BUFFER];
wchar_t pipe_parameter[CLIENT_ID_LIMIT];
char g_client_id[CLIENT_ID_LIMIT];


struct client {
	char clientId[CLIENT_ID_LIMIT];
	char filename[CLIENT_ID_LIMIT];
	size_t lenghtFile;
};
static void register_thread(HANDLE thread)
{
	EnterCriticalSection(&g_lock);
	if (g_threads_cnt + 1 > g_threads_max_cnt)
	{
		g_threads_max_cnt += 10;
		g_threads = (HANDLE*)realloc(g_threads, sizeof(HANDLE) * g_threads_max_cnt);
	}
	g_threads[g_threads_cnt] = thread;
	g_threads_cnt++;
	LeaveCriticalSection(&g_lock);
}
static HANDLE create_pipe(int first)
{
	HANDLE pipe;
	// Двунаправленная передача данных через канал
	DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
	if (first)
		open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
	pipe = CreateNamedPipe(server_name_pipe, // Имя канала
		open_mode,
		// Побайтовая передача данных, блокирующая
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		// Количество подключений не ограничено
		PIPE_UNLIMITED_INSTANCES,
		// Размеры буферов приема и передачи
		512, 512,
		0, // Таймаут по умолчанию
		NULL // Настройки безопасности
	);
	if (pipe == INVALID_HANDLE_VALUE || pipe == NULL)
	{
		printf("Error CreateNamedPipe(): %d\n", GetLastError());
		return 0;
	}
	return pipe;
}
DWORD WINAPI instance_thread(void* param)
{
	FILE *file = NULL;
	HANDLE pipe = (HANDLE)param;
	// Создание структуры OVERLAPPED для асинхронного чтения
	OVERLAPPED overlap;
	BOOL statusFirst = TRUE, statusProcessFile = FALSE;
	struct client clientCon;

	memset(&overlap, 0, sizeof(overlap));
	memset(&clientCon, 0x00, sizeof(struct client));
	overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	// Цикл обработки запросов клиентов
	while (1)
	{
		int rcv = 0;
		char buf[513];
		DWORD read;
		int i;
		// Чтение запроса из канала
		while (rcv < (sizeof(buf) - 1))
		{
			memset(buf, 0x00, 513);

			BOOL r = ReadFile(pipe, buf + rcv, sizeof(buf) - 1 - rcv, &read, &overlap);
			if (r)
			{
				// Операция чтения успешно завершена, данные получены
			}
			else if (!r && GetLastError() == ERROR_IO_PENDING)
			{
				// Операция начата, о завершении будет завершено через overlap.hEvent
				HANDLE wev[] = { overlap.hEvent, g_event_stop };
				DWORD w = WaitForMultipleObjects(2, wev, FALSE, INFINITE);
				if (w == WAIT_OBJECT_0)
				{
					// Операция чтения успешно завершена, получение количества принятых байт
					if (GetOverlappedResult(pipe, &overlap, &read, FALSE))
						r = TRUE;
				}
				else if (w == WAIT_OBJECT_0 + 1)
				{
					// Требуется завершение работы сервера => прервать чтение
					CancelIo(pipe);
					// Дождаться когда будет прервано
					WaitForSingleObject(overlap.hEvent, TRUE);
				}
			}
			if (!r)
			{
				// Освободить ресурсы, если данные не приянты, поток завершается
				EnterCriticalSection(&g_lock);
				sprintf(message, "%s %s %lu send-complete", clientCon.clientId, clientCon.filename, clientCon.lenghtFile);
				printf("Unknown error on client pipe while ReadFile(): %u. Client disconnected.\n", GetLastError());
				logMessage(message);
				LeaveCriticalSection(&g_lock);
				CloseHandle(overlap.hEvent);
				CloseHandle(pipe);
				fclose(file);
				return 0;
			}
			for (i = rcv; i < rcv + (int)read; i++)
			{
				if (i > 512) {
					break;
				}
				if (buf[i] == '\n')
				{
					rcv = sizeof(buf);
					if (statusFirst == TRUE)
					{
						char * point = strstr(buf, " ");
						char * pointBuf = buf;
						char sizeFile[100] = { 0x00 }, buffer[512] = { 0x00 };
						size_t readBytes;
						memcpy(clientCon.clientId, pointBuf, point - pointBuf);
						pointBuf += point - pointBuf + 1;
						point = strstr(pointBuf, " ");
						memcpy(clientCon.filename, pointBuf, point - pointBuf);
						pointBuf += point - pointBuf + 1;
						point = strstr(pointBuf, "\n");
						memcpy(sizeFile, pointBuf, point - pointBuf);
						clientCon.lenghtFile = atoi(sizeFile);
						memset(message, 0x00, 512);

						if (!strcmp(clientCon.filename, "stop")) {
							sprintf(message, "%s %s %lu", clientCon.clientId, clientCon.filename, clientCon.lenghtFile);
							logMessage(message);
							server_stop();
							return EXIT_SUCCESS;
						}

						if ((file = fopen(clientCon.filename, "wb")) == NULL)
						{
							printf("Error: can't open file %s", clientCon.filename);
						}
						sprintf(message, "%s %s %lu send-start", clientCon.clientId, clientCon.filename, clientCon.lenghtFile);
						logMessage(message);
						++point;
						readBytes = point - buf;
						memcpy(buffer, point, read - readBytes);
						memset(buf, 0x00, 513);
						memcpy(buf, buffer, read - readBytes);
						statusFirst = FALSE;
						read -= readBytes;
					}
					break;
				}
				rcv++;
			}
		}
		if (statusFirst == FALSE)
		{
			EnterCriticalSection(&g_lock);
			fwrite(buf, sizeof(BYTE), read, file);
			LeaveCriticalSection(&g_lock);
		}
	}
	fclose(file);
	return 0;
}
// Поток обрабатывает подключения клиентов и создание
// новых экземпляров канала в случае подключения клиентов
DWORD WINAPI server_thread(void* param)
{
	HANDLE pipe = (HANDLE)param;
	// Структура OVERLAPPED для выполнения асинхронной операции
	// ConnectNamedPipe
	OVERLAPPED overlap;
	memset(&overlap, 0, sizeof(overlap));
	overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	// Серверный цикл ожидания подключения клиентов
	while (1)
	{
		// Принятие подключения от клиента
		BOOL connected = ConnectNamedPipe(pipe, &overlap);
		if (connected || (!connected && (GetLastError() == ERROR_PIPE_CONNECTED)))
		{
			// Клиент успешно подключился
			connected = TRUE;
		}
		else
		{
			if (GetLastError() == ERROR_IO_PENDING)
			{
				// Функция начала ожидание подключения. Когда клиент подключится -
				// то overlapped.hEvent передет в сигнальное состояние
				HANDLE wev[] = { overlap.hEvent, g_event_stop };
				// Ожидание одного из событий: либо подключение клиента, либо СТОП сервера
				DWORD r = WaitForMultipleObjects(2, wev, FALSE, INFINITE);
				if (r == WAIT_OBJECT_0)
				{
					// Клиент подключился
					connected = TRUE;
				}
				else if (r == WAIT_OBJECT_0 + 1)
				{
					// Клиент не подключился => сервер должен быть остановлен
					// Прекратить OVELAPPED-операцию
					CancelIo(pipe);
					// Дождаться прекращения
					WaitForSingleObject(overlap.hEvent, INFINITE);
					// Закрыть все описатели
					CloseHandle(overlap.hEvent);
					CloseHandle(pipe);
					return 1;
				}
			}
			else
			{
				// Какая-то другая ошибка, которую мы не знаем и не можем обработать
				EnterCriticalSection(&g_lock);
				printf("Unknwon error on ConnectNamedPipe(): %u\n", GetLastError());
				LeaveCriticalSection(&g_lock);
			}
		}
		if (connected)
		{
			HANDLE thread;
			printf(" New client connected => new thread created\n");
			// Создание потока, обслуживающего подключившегося клиента
			thread = CreateThread(0, 0, instance_thread, (void*)pipe, 0, NULL);
			// Создание нового экземпляра канала - для подключения
			// следующего клиента.
			pipe = create_pipe(0);
		}
		else
		{
			// Если подключение не удалось - пересоздать экземпляр pipe
			CloseHandle(pipe);
			pipe = create_pipe(0);
		}
	}
	return 0;
}
void server_stop()
{
	unsigned i;
	SetEvent(g_event_stop);
	// Ожидание завершения всех потоков
	for (i = 0; i < g_threads_cnt; i++)
	{
		WaitForSingleObject(g_threads[i], INFINITE);
		CloseHandle(g_threads[i]);
	}
	return;
}
DWORD WINAPI stdin_thread(void* param)
{
	char buf[64];
	while (1)
	{
		memset(buf, 0, sizeof(buf));
		fgets(buf, sizeof(buf), stdin);
		while (strlen(buf) && buf[strlen(buf) - 1] == '\n')
			buf[strlen(buf) - 1] = 0;
		if (!strcmp(buf, "stop"))
		{
			EnterCriticalSection(&g_lock);
			printf("Exiting...\n");
			LeaveCriticalSection(&g_lock);
			server_stop();
			break;
		}
	}
	return 0;
}
int main(int argc, char * argv[])
{
	if (argc < 3 || argc > 4)
	{
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if ((transferFile = fopen(TRANSFER_FILE_NAME, "w")) == NULL) {
		printf("Error: can't open file %s", TRANSFER_FILE_NAME);
	}
	parseArguments(argv);
}
void logMessage(char *message)
{
	time_t rawtime;
	struct tm * timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	fprintf(transferFile, "%d:%d:%d %s\n", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, message);
}
void usage(char *programName)
{
	fprintf(stdout, "Usage: %s server <name pipe> for server format \n"
		"%s <id client> <name pipe>\n", programName, programName);
}
void parseArguments(char **argv)
{
	memset(server_name_pipe, 0x00, sizeof(wchar_t) * SIZE_BUFFER);
	memset(pipe_parameter, 0x00, sizeof(wchar_t) * CLIENT_ID_LIMIT);
	wcscpy(server_name_pipe, SERVER_PIPE_PREFIX);
	mbstowcs(pipe_parameter, argv[2], strlen(argv[2]));
	wcscat(server_name_pipe, pipe_parameter);

	if (!strncmp(argv[1], SERVER_FORMAT, strlen(SERVER_FORMAT)))
	{
		startServer();
	}
	else {
		sprintf(g_client_id, "%s", argv[1]);
		startClient(argv[3]);
	}
}

int startServer(void)
{
	// Создание первого экземпляра канала
	HANDLE pipe = create_pipe(1);
	HANDLE serv_thread;
	if (!pipe)
		return -1;
	// Событие с "ручным" сбросом: если его перевести в сигнальное состояние,
	// то оно в нем так и останется, даже после WaitFor..
	g_event_stop = CreateEvent(0, TRUE, FALSE, NULL);
	InitializeCriticalSection(&g_lock);
	printf("Listening pipe...\n");
	serv_thread = CreateThread(NULL, 0, server_thread, (void*)pipe, 0, NULL);
	register_thread(serv_thread);
	while (1)
	{
		if (WAIT_OBJECT_0 == WaitForSingleObject(serv_thread, 2000))
		{
			fclose(transferFile);
			break;
		}
	}
	CloseHandle(g_event_stop);
	DeleteCriticalSection(&g_lock);
	free(g_threads);
	return 0;
}
int startClient(char *filename)
{
	HANDLE pipe;
	BOOL b;
	OVERLAPPED overlap;
	FILE *file = NULL;
	size_t lenghtFile = 0;
	char buf[513];
	char *point = NULL;
	DWORD written;

	if (strcmp(filename, "stop"))
	{
		if ((file = fopen(filename, "rb")) == NULL)
		{
			printf("Error: can't open file %s", filename);
			return EXIT_FAILURE;
		}
		fseek(file, 0, SEEK_END);                                      // переместить внутренний указатель в конец файла
		lenghtFile = ftell(file);
		rewind(file);
	}

	g_event_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
	// Подключение к серверному каналу
	printf("Connecting to server...\n");
	pipe = INVALID_HANDLE_VALUE;
	do
	{
		// Ожидание появления свободного экземпляра канала
		if (WaitNamedPipe(server_name_pipe, 100))
		{
			pipe = CreateFile(server_name_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL,
				OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		}
	} while (pipe == INVALID_HANDLE_VALUE);
	printf("Connected.\n");

	memset(&overlap, 0, sizeof(overlap));
	overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	printf("Sending messages...\n");

	// Отправка запроса
	point = strrchr(filename, '\\');
	if (point == NULL)
	{
		point = filename;
	}
	else {
		++point;
	}
	sprintf_s(buf, sizeof(buf), "%s %s %lu\n", g_client_id, point, lenghtFile);
	b = WriteFile(pipe, buf, strlen(buf), &written, &overlap);
	if (b == TRUE)
	{
		// Запись в канал завершена немедленно
	}
	else if (!b && (GetLastError() == ERROR_IO_PENDING))
	{
		// Запись в канал начата, будет завершена позже, ожидание этого
		HANDLE wev[] = { overlap.hEvent, g_event_stop };
		DWORD w = WaitForMultipleObjects(2, wev, FALSE, INFINITE);
		if (w == WAIT_OBJECT_0)
		{
			if (GetOverlappedResult(pipe, &overlap, &written, FALSE))
				b = TRUE;
		}
		else if (w == WAIT_OBJECT_0 + 1)
		{
			// Поступило событие завершения программы
			CancelIo(pipe); // Отменить передачу
			WaitForSingleObject(overlap.hEvent, INFINITE); // Дождаться отмены
			return EXIT_SUCCESS;
		}
	}
	if (!b)
	{
		printf("Failed WriteFile(): %u.\n", GetLastError());
		return EXIT_FAILURE;
	}

	if (file != NULL)
	{
		while (!feof(file))
		{
			size_t readBytes = 0;
			printf("Sending messages...\n");

			readBytes = fread(buf, sizeof(BYTE), 512, file);
			// Отправка запроса
			b = WriteFile(pipe, buf, readBytes, &written, &overlap);
			if (b == TRUE)
			{
				// Запись в канал завершена немедленно
			}
			else if (!b && (GetLastError() == ERROR_IO_PENDING))
			{
				// Запись в канал начата, будет завершена позже, ожидание этого
				HANDLE wev[] = { overlap.hEvent, g_event_stop };
				DWORD w = WaitForMultipleObjects(2, wev, FALSE, INFINITE);
				if (w == WAIT_OBJECT_0)
				{
					if (GetOverlappedResult(pipe, &overlap, &written, FALSE))
						b = TRUE;
				}
				else if (w == WAIT_OBJECT_0 + 1)
				{
					// Поступило событие завершения программы
					CancelIo(pipe); // Отменить передачу
					WaitForSingleObject(overlap.hEvent, INFINITE); // Дождаться отмены
					break;
				}
			}
			if (!b)
			{
				printf("Failed WriteFile(): %u.\n", GetLastError());
				break;
			}
		}
		fclose(file);
	}
	CloseHandle(pipe);
	CloseHandle(overlap.hEvent);
	CloseHandle(g_event_stop);
	return EXIT_SUCCESS;
}