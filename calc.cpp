#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <Windows.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>


// ��� �������, ������� ����� ��������� ������� � ����������
// ���������, ���� �� ���-�� ������� � ����� ������
#define SHMEM_SRV_EVENT TEXT("ShmemSrvEvent")
// ��� �������, ������� ���������� "���������" ����� ���
// ��� ���-���� ������ � ����� �������
#define SHMEM_SRV_MUTEX TEXT("ShmemSrvMutex")

// ������������ ����� ����� �������
#define MAX_CLIENTID (16)
// ������������� ����� ���������
#define MAX_CLIENTMSG (128)
// ����������� ��������� � �������
#define MAX_MSG_QUEUE (20)
// ��������� ��� ������ �������

#define CLIENT_ID_LIMIT			64
#define SERVER_FORMAT			"server"
#define CALC_FILE_NAME			"request.txt"
#define SIZE_BUFFER				512



struct calculator_element
{
	int id;
	int value;
	char operation;
	struct calculator_element *next;
};

enum shmem_srv_msgtype
{
	shmem_srv_msgtype_none = 0, // ��� ���������
	shmem_srv_msgtype_answer = 1, // ������ ����������� ����������
	shmem_srv_msgtype_stop = 2 // ������ �����������
};
struct shmem_srv_msg
{
	enum shmem_srv_msgtype msgtype;
	char answer[128];
};
enum shmem_cli_msgtype
{
	shmem_cli_msgtype_none = 0, // ��� ���������
	shmem_cli_msgtype_connect = 1,
	shmem_cli_msgtype_disconnect = 2,
	shmem_cli_msgtype_message
};
struct shmem_cli_msg
{
	volatile enum shmem_cli_msgtype msgtype;
	// ������������� �������
	volatile char cli_id[MAX_CLIENTID + 1];
	// ��������� �� ������� (��� ��� �������)
	volatile char cli_msg[MAX_CLIENTMSG + 1];
};
// ����������� ������� ������. �� ���� ��� ������� �
// ������ ����� ����� ��������� �� ����� ��������� (shmem) �
// ����� � ��� �������� ������������, ��� ���� ����������
// ������ SHMEM_SRV_MUTEX
struct shmem
{
	// ��������� ����� �������
	volatile struct shmem_srv_msg srv_msg;
	// �������������� ��������� �� ��������
	volatile struct shmem_cli_msg cli_msgs[MAX_MSG_QUEUE];
};
// ���������� � ������� ��������
struct cli_info
{
	char cli_id[MAX_CLIENTID + 1];
	// �������, ������� ��������� ������� � ����� ������
	HANDLE cli_ev;
	struct cli_info* next;
	struct cli_info* prev;
};


static struct cli_info* g_clients = 0;
static ULONG g_clients_cnt = 0;

FILE *requestFile = NULL;
char answer[128];
char message[SIZE_BUFFER];
// ������������� �������� �������
char g_cli_id[MAX_CLIENTID + 1];
// ��� ������� �������� ������� (��� ����������� �� �������)
char g_my_ev_nm[128];
// ��������� �� ����������� ������� ������
struct shmem* g_shmem;
// ������ ��� ������������������ ������� � g_shmem
HANDLE g_mem_mutex;
// �������, ������� ������� ���������� � ����������
// ��������� ����� �������� ���������
HANDLE g_mem_event;
wchar_t shmem_srv_mapping[SIZE_BUFFER];
struct calculator_element *root = NULL;
char expression[MAX_CLIENTMSG];


int calculate(void);
int calculateHighPriority();
int calculateMiddlePriority();
int calculateLowPriority();
int check_and_sum();
int parseString(char *);
void usage(char *);
void parseArguments(char **);
int startServer(char *);
int startClient(void);
void logMessage(char *);
int sendMessages(void);
static int transfer_msg_to_serv(struct shmem_cli_msg*);
int treatmentMessage(struct shmem_cli_msg *);
static void shm_srv_answer(char*, struct shmem*);
static void shm_srv_broadcast_stop(struct shmem*);

static void cli_add(const char* cli_id, const char* cli_ev_nm)
{
	struct cli_info* cur = g_clients;
	// �������� ������� ������� � ����� ������
	while (cur)
	{
		if (!strcmp(cli_id, cur->cli_id))
		{
			// ������ ��� ����, ����������� Event � �����
			printf("Client reconnected: '%s'\n", cli_id);
			cur->cli_ev = OpenEventA(EVENT_ALL_ACCESS, FALSE, cli_ev_nm);
			if (cur->cli_ev == NULL)
				printf("OpenEvent failed: %u.\n", GetLastError());
		}
		cur = cur->next;
	}
	// ��������� ������ ��� ������ �������� ������.

	printf("Client '%s' connected.\n", cli_id);
	cur = (struct cli_info*) malloc(sizeof(struct cli_info));
	strcpy_s(cur->cli_id, sizeof(cur->cli_id), cli_id);
	// �������� �������, ��� �������� ������������� ��������
	cur->cli_ev = OpenEventA(EVENT_ALL_ACCESS, FALSE, cli_ev_nm);
	if (cur->cli_ev == NULL)
	{
		printf("OpenEvent failed: %u.\n", GetLastError());
		free(cur);
		return;
	}
	memset(message, 0x00, SIZE_BUFFER);
	sprintf(message, "%s connect", cli_id);
	logMessage(message);
	// ������� ������� � ������
	cur->prev = NULL;
	cur->next = g_clients;
	g_clients = cur;
	g_clients_cnt++;
}
static void cli_rem(const char* cli_id)
{
	struct cli_info* cur = g_clients;
	while (cur)
	{
		if (!strcmp(cur->cli_id, cli_id))
		{
			if (cur->prev)
				cur->prev->next = cur->next;
			else
				g_clients = cur->next;
			if (cur->next)
				cur->next->prev = cur->prev;
			printf("Client '%s' removed.\n", cli_id);
			memset(message, 0x00, SIZE_BUFFER);
			sprintf(message, "%s disconnet", cli_id);
			logMessage(message);
			// ������� �������, ���������� ������
			CloseHandle(cur->cli_ev);
			free(cur);
			g_clients_cnt--;
			return;
		}
		cur = cur->next;
	}
}

// ������� ��������� ��� ����������� ��������� �� ��������.
// ������ ���������� ����� ������ SHMEM_SRV_MUTEX ��������
int shm_srv_pop_messages(struct shmem* shmem)
{
	ULONG i;

	// ���� �������� �� ���� ������, � ���� � ����� ���� ��������� -
	// ������������ ��� � ������� �� �����
	for (i = 0; i < MAX_MSG_QUEUE; i++)
	{
		volatile struct shmem_cli_msg* cli_msg = &shmem->cli_msgs[i];
		if (cli_msg->msgtype == shmem_cli_msgtype_connect)
		{
			// ������ �����������, �������� � ������
			cli_add((const char*)cli_msg->cli_id, (const char*)&cli_msg->cli_msg[0]);
		}
		else if (cli_msg->msgtype == shmem_cli_msgtype_disconnect)
		{
			// ������ ����������, ������ �� ������
			cli_rem((const char*)cli_msg->cli_id);
		}
		else if (cli_msg->msgtype == shmem_cli_msgtype_message)
		{

			// ������ ������� ���������
			if (treatmentMessage((struct shmem_cli_msg *)cli_msg) == EXIT_FAILURE)
			{
				memset(message, 0x00, SIZE_BUFFER);
				sprintf(message, "%s stop", (char *)cli_msg->cli_id);
				logMessage(message);
				shm_srv_broadcast_stop(shmem);
				return EXIT_FAILURE;
			}
			else
			{
				memset(message, 0x00, SIZE_BUFFER);
				sprintf(message, "%s %s", (char *)cli_msg->cli_id, (char*)cli_msg->cli_msg);
				logMessage(message);
				shm_srv_answer((char *)cli_msg->cli_id, shmem);
			}

		}
		// �������, ��� "����" ������ ��������
		cli_msg->msgtype = shmem_cli_msgtype_none;
	}
	return EXIT_SUCCESS;
}
// ������� ��������� ������� ���� �������� � ���������� ���������.
// ��� ���� ������� ������� ��� ���������
static void shm_srv_notify_clients()
{
	struct cli_info* cur = g_clients;
	while (cur)
	{
		SetEvent(cur->cli_ev);
		cur = cur->next;
	}
}

static void shm_srv_answer(char* cli_id, struct shmem* shmem)
{
	struct cli_info* cur = g_clients;
	shmem->srv_msg.msgtype = shmem_srv_msgtype_answer;
	strcpy((char*)shmem->srv_msg.answer, answer);
	while (cur)
	{
		if (!strcmp(cur->cli_id, cli_id))
		{
			SetEvent(cur->cli_ev);
			break;
		}
		cur = cur->next;
	}
}

// ��������� ����������� �� ��������� ���� ��������.
// ������ ���������� ����� ������ SHMEM_SRV_MUTEX ��������
static void shm_srv_broadcast_stop(struct shmem* shmem)
{
	shmem->srv_msg.msgtype = shmem_srv_msgtype_stop;
	shm_srv_notify_clients();
	printf("broadcast_stop send.\n");
}
int main(int argc, char * argv[])
{
	if (argc < 3 || argc > 4)
	{
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if ((requestFile = fopen(CALC_FILE_NAME, "w")) == NULL) {
		printf("Error: can't open file %s", CALC_FILE_NAME);
	}
	parseArguments(argv);

}
int treatmentMessage(struct shmem_cli_msg *shmem)
{
	if (!strcmp((char*)shmem->cli_msg, "stop"))
	{
		return EXIT_FAILURE;
	}
	parseString((char*)shmem->cli_msg);
	return EXIT_SUCCESS;
}
void logMessage(char *message)
{
	time_t rawtime;
	struct tm * timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	fprintf(requestFile, "%d:%d:%d %s\n", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, message);
}

void usage(char *programName)
{
	fprintf(stdout, "Usage: %s server <file map> for server format \n"
		"%s <id client> <file map>\n", programName, programName);
}
void parseArguments(char **argv)
{
	memset(shmem_srv_mapping, 0x00, sizeof(wchar_t) * SIZE_BUFFER);
	mbstowcs(shmem_srv_mapping, argv[2], strlen(argv[2]));

	if (!strncmp(argv[1], SERVER_FORMAT, strlen(SERVER_FORMAT)))
	{
		startServer(argv[2]);
	}
	else {
		sprintf(g_cli_id, "%s", argv[1]);
		sprintf(expression, "%s", argv[3]);
		startClient();
	}
}

int startServer(char *srv_mailslot_parameter)
{
	HANDLE mem_event;
	HANDLE mem_mutex;
	HANDLE mem_mapping;
	struct shmem* shmem;
	// �������� �������� ������������� ��� ������ � ����� �������
	mem_event = CreateEvent(NULL, TRUE, FALSE, SHMEM_SRV_EVENT);
	if (mem_event == NULL)
	{
		printf("CreateEvent() error: %u.\n", GetLastError());
		return -1;
	}
	mem_mutex = CreateMutex(NULL, FALSE, SHMEM_SRV_MUTEX);
	if (mem_mutex == NULL)
	{
		printf("CreateMutex() error: %u.\n", GetLastError());
		return -1;
	}
	// ������ �������
	WaitForSingleObject(mem_mutex, INFINITE);
	// �������� ������� "����������� ������"
	mem_mapping = CreateFileMapping(
		INVALID_HANDLE_VALUE, // ������������ ��������� pagefile
		NULL, // �������� ������������ �� ���������
		PAGE_READWRITE, // ������ �� ������ � ������
		0, // max ������ (high)
		sizeof(struct shmem), // max ������ (low)
		shmem_srv_mapping // ��� �������
	);
	if (mem_mapping == NULL)
	{
		printf("Failed CreateFileMapping(): %d\n", GetLastError());
		return -1;
	}
	// ����������� �������� ������
	shmem = (struct shmem*) MapViewOfFile(mem_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct
		shmem));
	if (!shmem)
	{
		printf("MapViewOfFile failed: %u.\n", GetLastError());
		CloseHandle(mem_mapping);
		return -1;
	}
	// ��������� ��������
	memset(shmem, 0, sizeof(*shmem));
	// ������������ �������, �.�. ������ ����� �������� ������
	ReleaseMutex(mem_mutex);
	printf("Waiting messages from clients...\n");
	while (TRUE)
	{
		HANDLE wev[] = { mem_event };
		DWORD wr;
		wr = WaitForMultipleObjects(1, wev, FALSE, 5000);
		if (wr == WAIT_OBJECT_0)
		{
			// ������� mem_event �������� => ������� ���-�� ���������
			// �������� ������
			WaitForSingleObject(mem_mutex, INFINITE);
			// ������� event
			ResetEvent(mem_event);
			if (shm_srv_pop_messages(shmem) == EXIT_FAILURE)
			{
				ReleaseMutex(mem_mutex);
				break;
			}
			// ��������� ������
			ReleaseMutex(mem_mutex);
		}
	}
	if (!UnmapViewOfFile(shmem))
	{
		printf("UnmapViewFile error: %u.\n", GetLastError());
	}
	CloseHandle(mem_mapping);
	CloseHandle(mem_event);
	CloseHandle(mem_mutex);
	return EXIT_SUCCESS;
}

int startClient()
{
	HANDLE my_notify_ev; // ������� ��� �������� �� �������
	HANDLE mem_mapping;

	sprintf(g_my_ev_nm, "ShmemCli%s", g_cli_id);
	// ����� ANSI-������ ������� CreateEvent, �.�. ��� ������� � char, � �� wchar_t
	// ������ char, �.�. ��� ������� ����� ���������� �������.
	// ������� ���������� � ���������� ��������� - ����� ������ ��������� ���������
	// ����� �������
	my_notify_ev = CreateEventA(NULL, FALSE, TRUE, g_my_ev_nm);
	if (my_notify_ev != NULL)
	{
		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			// ������� � ����� ������ ��� ����
			CloseHandle(my_notify_ev);
			my_notify_ev = NULL;
			return EXIT_FAILURE;
		}

		printf("ClientID: '%s'\n", g_cli_id);
	}
	if (my_notify_ev == NULL)
	{
		printf("Failed CreateEventA(): %u\n", GetLastError());
		return -1;
	}
	// ����������� �������� ����������� ������ (�� �����)
	mem_mapping = OpenFileMapping(GENERIC_ALL, 0, shmem_srv_mapping);
	if (mem_mapping == NULL)
	{
		printf("Failed OpenFileMapping(): %u\n", GetLastError());
		return -1;
	}
	g_shmem = (struct shmem*) MapViewOfFile(mem_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct
		shmem));
	if (g_shmem == NULL)
	{
		printf("Failed MapViewOfFile(): %u\n", GetLastError());
		return -1;
	}
	// �������� �������� �������������
	g_mem_event = OpenEvent(GENERIC_ALL, FALSE, SHMEM_SRV_EVENT);
	if (g_mem_event == NULL)
	{
		printf("Failed OpenEvent(): %u\n", GetLastError());
		return -1;
	}
	g_mem_mutex = OpenMutex(GENERIC_ALL, FALSE, SHMEM_SRV_MUTEX);
	if (g_mem_mutex == NULL)
	{
		printf("Failed OpenMutex(): %u\n", GetLastError());
		return -1;
	}

	// ���������� ���������� ������ �� ������
	sendMessages();
	//send_thread = CreateThread(NULL, 0, &sendMessages, NULL, 0, NULL);
	while (TRUE)
	{
		// ������ ����� ����� �� �������
		WaitForSingleObject(g_mem_mutex, INFINITE);
		if (g_shmem->srv_msg.msgtype == shmem_srv_msgtype_answer)
		{
			struct shmem_cli_msg cli_msg;
			// ������ ���������� - ���������� �� �����
			printf("%s", g_shmem->srv_msg.answer);
			strcpy((char*)cli_msg.cli_id, g_cli_id);
			cli_msg.msgtype = shmem_cli_msgtype_disconnect;
			transfer_msg_to_serv(&cli_msg);
			break;
		}
		else if (g_shmem->srv_msg.msgtype == shmem_srv_msgtype_stop)
		{
			// ������ �����������
			printf("Server stopped.\n");
			ReleaseMutex(g_mem_mutex);
			break;
		}
		ReleaseMutex(g_mem_mutex);
	}
	CloseHandle(g_mem_mutex);
	CloseHandle(g_mem_event);
	UnmapViewOfFile(g_shmem);
	CloseHandle(mem_mapping);
	CloseHandle(my_notify_ev);
	return 0;

}

int sendMessages(void)
{
	char buf[128] = { 0 };
	struct shmem_cli_msg cli_msg;
	// �������� ������� ���������� � ����������� � ����� �������
	strcpy((char*)cli_msg.cli_id, g_cli_id);
	cli_msg.msgtype = shmem_cli_msgtype_connect;
	strcpy((char*)cli_msg.cli_msg, g_my_ev_nm);
	if (!transfer_msg_to_serv(&cli_msg))
		return -1;

	strcpy((char*)cli_msg.cli_id, g_cli_id);
	cli_msg.msgtype = shmem_cli_msgtype_message;
	strcpy((char*)cli_msg.cli_msg, expression);
	if (transfer_msg_to_serv(&cli_msg))
		printf("Message send.\n");
	return 0;
}
// ������� ��������� �������� ��������� �� ������
// (���� ���� ��������� ����� � �������� ����� ������)
static int transfer_msg_to_serv(struct shmem_cli_msg* msg)
{
	while (TRUE)
	{
		ULONG i;
		// ������ ������� ����� ����� ���� �������� � �������
		WaitForSingleObject(g_mem_mutex, INFINITE);
		if (g_shmem->srv_msg.msgtype == shmem_srv_msgtype_stop)
		{
			// ������ �����������, ������� 0
			ReleaseMutex(g_mem_mutex);
			return 0;
		}
		// ����� ���������� ����� ��� ������ ���������
		for (i = 0; i < MAX_MSG_QUEUE; i++)
		{
			if (g_shmem->cli_msgs[i].msgtype == shmem_cli_msgtype_none)
			{
				// ������ ��������� � ����� ������
				memcpy((void*)&g_shmem->cli_msgs[i], msg, sizeof(struct shmem_cli_msg));
				// ����������� ������� �� ����
				SetEvent(g_mem_event);
				// ������������ ������� � �����
				ReleaseMutex(g_mem_mutex);
				return 1;
			}
		}
		// ��������� ������ ���, ���������, ���������
		ReleaseMutex(g_mem_mutex);
		Sleep(100);
	}
	return 1;
}

int check_and_sum()
{
	struct calculator_element* cur = root;
	int result = 0;
	while (cur)
	{
		result += cur->value;
		cur = cur->next;
	}
	return result;
}
void element_add(char operation, int value)
{
	struct calculator_element* c = (struct calculator_element*) malloc(sizeof(struct calculator_element));
	struct calculator_element* cur = root, *prev;
	memset(c, 0x00, sizeof(struct calculator_element));

	c->operation = operation;
	c->value = value;
	c->next = NULL;

	if (root == NULL) {
		c->id = 0;
		root = c;
	}
	else
	{
		while (cur != NULL)
		{
			prev = cur;
			cur = cur->next;
		}
		prev->next = c;
		c->id = prev->id + 1;
	}

}
struct calculator_element* element_remove(int id)
{
	struct calculator_element* cur = root, *prev = NULL;
	while (cur != NULL && cur->id != id)
	{
		prev = cur;
		cur = cur->next;
	}
	if (prev == NULL)
	{
		root = cur->next;
		free(cur);
		return root;
	}
	else {
		prev->next = cur->next;
		free(cur);
		return root;
	}
}
struct calculator_element* element_add_and_remove(char operation, int value, int id)
{
	struct calculator_element* c = (struct calculator_element*) malloc(sizeof(struct calculator_element));
	struct calculator_element* cur = root, *prev = NULL;
	memset(c, 0x00, sizeof(struct calculator_element));
	c->id = id;
	c->operation = operation;
	c->value = value;
	c->next = NULL;
	while (cur != NULL && cur->id != id)
	{
		prev = cur;
		cur = cur->next;
	}
	if (prev == NULL)
	{
		c->next = cur->next;
		root = c;
		free(cur);
		return root;
	}
	else {
		c->next = cur->next;
		prev->next = c;
		free(cur);
		return root;
	}
}
int parseString(char *input)
{

	char number_string[30];
	char op = 0;

	unsigned int index = 0;
	unsigned int to = 0;
	size_t input_length = 0;
	unsigned int number_length = 0;
	int result = 0, number = 0;

	sprintf(answer, "error\n");
	input_length = strlen(input);

	for (to = 0, index = 0; index <= input_length; index++)
		if (*(input + index) != ' ')
			*(input + to++) = *(input + index);
	input_length = strlen(input);
	index = 0;
	if (input[index] == '=')
		return EXIT_FAILURE;
	else
	{
		number_length = 0;
		if (input[index] == '+' || input[index] == '-')
			*(number_string + number_length++) = *(input + index++);
		for (; isdigit(*(input + index)); index++)
			*(number_string + number_length++) = *(input + index);
		*(number_string + number_length) = '\0';
		if (number_length>0)
			result = atoi(number_string);
	}
	element_add(0x00, result);
	for (; index < input_length;)
	{
		op = *(input + index++);
		number_length = 0;
		if (input[index] == '+' || input[index] == '-')
			*(number_string + number_length++) = *(input + index++);
		for (; isdigit(*(input + index)); index++)
			*(number_string + number_length++) = *(input + index);

		*(number_string + number_length) = '\0';
		number = atoi(number_string);

		switch (op)
		{
		case '+':
			element_add(op, number);
			break;
		case '-':
			element_add(op, 0 - number);
			break;
		case '*':
			element_add(op, number);
			break;
		case '/':
			if (number == 0)
				return EXIT_FAILURE;
			else
				element_add(op, number);
			break;
		case '%':
			if ((long)number == 0)
				return EXIT_FAILURE;
			else
				element_add(op, number);
			break;
		default:
			return EXIT_FAILURE;
		}
	}
	result = calculate();
	free(root);
	root = NULL;
	sprintf(answer, "%d\n", result);
	return EXIT_SUCCESS;
}
int calculate(void)
{
	calculateHighPriority();
	calculateMiddlePriority();
	calculateLowPriority();
	return check_and_sum();
}
int calculateHighPriority(void)
{
	struct calculator_element* current = root, *prev, *element = NULL;
	while (current)
	{
		int id = current->id;
		if (current->operation == '*')
		{
			current = element_add_and_remove(prev->operation, prev->value * current->value, prev->id);
			current = element_remove(id);
		}
		else if (current->operation == '/')
		{
			current = element_add_and_remove(prev->operation, prev->value / current->value, prev->id);
			current = element_remove(id);
		}
		prev = current;
		current = current->next;
	}
	return EXIT_SUCCESS;
}
int calculateMiddlePriority(void)
{
	struct calculator_element* current = root, *prev, *element = NULL;
	while (current)
	{
		int id = current->id;
		if (current->operation == '%')
		{
			current = element_add_and_remove(prev->operation, prev->value % current->value, prev->id);
			current = element_remove(id);
		}
		prev = current;
		current = current->next;
	}
	return EXIT_SUCCESS;
}
int calculateLowPriority(void)
{
	struct calculator_element* current = root, *prev, *element = NULL;
	while (current)
	{
		int id = current->id;
		if (current->operation == '-')
		{
			current = element_add_and_remove(prev->operation, prev->value + current->value, prev->id);
			current = element_remove(id);

		}
		else if (current->operation == '+')
		{
			current = element_add_and_remove(prev->operation, prev->value + current->value, prev->id);
			current = element_remove(id);
		}
		prev = current;
		current = current->next;
	}
	return EXIT_SUCCESS;
}