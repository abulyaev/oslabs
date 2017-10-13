#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>

// Директива линковщику: использовать библиотеку сокетов   
#pragma comment(lib, "ws2_32.lib") 

#include <stdio.h>

enum errors {					// Список значений, которые могут возвращать функции
	no_error,						// Без ошибочное завершение функции
	wrong_ack,						// Ошибочный номер отваета
	wrong_syn,						// Ошибочный номер запроса
	wrong_accept,					// Ошибка функции accept (подключение tcp клиента)
	wrong_tcp_type,					// TCP сообщение с неизвестным кодом запроса
	wrong_udp_type,					// UDP сообщение с неизвестным кодом запроса
	stop_message,					// Зафиксированно сообщение stop
	connect_error,					// Ошибка подключения
	timeout,						// Таймер истек (используется при инициализации повторной отправки сообщений)
	file_not_found,					// Файл с введенным именем не найден
	send_error,						// Ошибка отправки
	no_client,						// Возвращается при попытке отключения не "зарегестрированного" клиента
	dyplicate_message				// Возвращается если пришло два одинаковых сообщений
};

enum type_tcp_packets {			// Типы TCP сообщений
	short_message,					// Сообщение с "коростким сообщением"
	recv_file_list,					// Сообщение с запросом списка элементов в папке
	end_file_list,					// Отправляется после отправки последнего элемента в папке
	reply_list_file					// Ответ на сообщение с запросом списка элементов
};

enum type_udp_packets {			// Типы UDP сообщений
	syn, syn_ack,					// Флаги для инициализации сессии (syn - запрос на инициализацию сессии (от клиента), syn_ack - подтверждение инициализации сессии (от сервера))
	ack,							// Подтверждение сообщения (отправляется на каждое принятое сообщение)
	send_info_file,					// Отправка информации о файле (имени и размера) на сервер
	send_part_file,					// Отправка части файла на сервер
	recv_info_file,					// Запрос на отправку файла с сервера
	recv_part_file,					// Запрос на отправку части файла с сервера
	recv_end,						// Данный флаг передаётся после отправки всех частей файла
	wrong_file_name,				// Сообщает клиенту чо файл с указанным именем не найден
	fin, fin_ack					// Флаги для завершения сессии (fin отправляется клиентом для завершения сессии, fin_ack - подтверждение от сервера)
};

struct udp_packet {				// Структура UDP пакета
	unsigned short syn;				// Номер сообщения отправленного клиентом в рамках сессии
	unsigned short ack;				// Номер сообщения отправленного сервером в рамках сессии
	type_udp_packets type;			// Тип сообщения (смотри type_udp_packets)
	union {							// Типы полезной нагрузки
		char  buffer[4000];				// Просто 4000 байт
		struct send_info_file {			// Информация о файле
			unsigned long long size;
			char name[4000 - sizeof(unsigned long long)];
		} info_file;
	} payload;
};

struct tcp_packet {				// Структура TCP пакета
	type_tcp_packets type;			// Тип сообщения (смотри type_tcp_packets)
	char  buffer[4000];				// 4000 байт полезной нагрузки
};

unsigned short syn_number, ack_number;	// Количества сообщений отправленых клиентом и клиенту

										// Отправка UDP пакета
errors udp_send(int socket, sockaddr_in addr, udp_packet *packet, int len) {
	packet->syn = syn_number;
	packet->ack = ack_number;
	sendto(socket, (char*)packet, len, 0, (sockaddr*)&addr, sizeof(sockaddr_in));
	return no_error;
};

// Приём UDP пакета
errors udp_recv(int socket, sockaddr_in addr, udp_packet *packet, int *size, int time_wait) {
	WSAEVENT events;									// Инициализация события для конченого ожидания сообщения
	int addrlen = sizeof(sockaddr);

	events = WSACreateEvent();
	WSAEventSelect(socket, events, FD_READ);

	WSANETWORKEVENTS ne;								// Ожидание сообщения в течении time_wait
	DWORD dw = WSAWaitForMultipleEvents(1, &events, FALSE, time_wait, FALSE);
	WSAResetEvent(events);
	// Если сообщений не приходило в течении time_wait
	if (0 != WSAEnumNetworkEvents(socket, events, &ne) || !(ne.lNetworkEvents & FD_READ))
		return timeout;
	// Получение сообщения
	*size = recvfrom(socket, (char*)packet, sizeof(udp_packet), 0, (struct sockaddr*) &addr, &addrlen);
	if (packet->ack != ack_number + 1)					// Проверка номеров сообщений, сервер должен увеличить количество подтверждений/ответов (ack_number) и оставить без изменения количество запросов (syn_number)
		return wrong_ack;
	if (packet->syn != syn_number)
		return wrong_syn;
	++ack_number;
	++syn_number;
	return no_error;
};

// Отправка сообщения и ожидания его подверждения
errors udp_send_with_ack(int socket, sockaddr_in addr, udp_packet *packet, int *size, int len) {
	int i;
	udp_packet recv_packet;

	for (i = 0; i < 5; ++i) {
		udp_send(socket, addr, packet, len);			// Отправляем сообщение
		if (udp_recv(socket, addr, &recv_packet, size, 200) == no_error)	// Если пришёл ответ 
			if (recv_packet.type == ack)										// и он имеет тип подтверждения (ack)
				break;															// Завершаем отправку
	}													// Иначе повторяем (максимум 5 раз)

	if (i == 5)											// Если все 5 попыток не удачны, то возвращаем ошибку
		return send_error;
	return no_error;
}

// Отправка сообщения и ожидания ответа на него (аналог предыдущей функции, только ответ (recv_packet) выходное значение)
errors udp_send_with_reply(int socket, sockaddr_in addr, udp_packet *packet, int *size,
	udp_packet *recv_packet, int len) {
	int i;

	for (i = 0; i < 5; ++i) {
		udp_send(socket, addr, packet, len);
		if (udp_recv(socket, addr, recv_packet, size, 200) == no_error)
			if (recv_packet->type == ack || recv_packet->type == wrong_file_name)
				break;
	}

	if (i == 5)
		return send_error;
	return no_error;
}

int sock_err(const char* function, int s) {
	int err;
	err = WSAGetLastError();
	fprintf(stderr, "%s: socket error: %d\n", function, err);
	return -1;
}

// Функция для получения размера файла
errors get_file_size(char *name, unsigned long long* size) {
	HANDLE handl;
	WIN32_FIND_DATA desc;														// Структура с описанием некоторых параметров объекта файловой системы, нам важен размер
	handl = FindFirstFile(name, &desc);											// Находим файл с заданным именем

	if (handl == NULL)
		return file_not_found;
	*size = ((unsigned long long)desc.nFileSizeHigh << 32) + desc.nFileSizeLow;	// Размер 64 бита, хранится в двух 32 битныйх полях структуры (в nFileSizeHigh - старшие 32 бита, nFileSizeLow - младшие 32)
	return no_error;
}

// Инициализация tcp соединения
errors connect_tcp_socket(int* sock, sockaddr_in addr) {
	*sock = socket(AF_INET, SOCK_STREAM, 0);   // Создание TCP-сокета  

											   // Установка соединения с удаленным хостом  
	if (connect(*sock, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
		closesocket(*sock);
		sock_err("connect", *sock);
		return connect_error;
	}
	return no_error;
}

errors connect_udp_socket(int* sock, sockaddr_in addr) {
	udp_packet packet;
	int i, size;

	*sock = socket(AF_INET, SOCK_DGRAM, 0);   // Создание UDP-сокета  

											  // Установка соединения с удаленным хостом  
	syn_number = 0;							// Выставляем начальное значение "количеству переданых серверу сообщений"
	ack_number = 0;							// Выставляем начальное значение "количеству переданых сервером сообщений"

	for (i = 0; i < 5; ++i) {				// Отпраквка сообщения с подтверждением (аналог функции udp_send_with_ack, но подтверждение синхронизации имеет тип syn_ack)
		packet.type = syn;
		udp_send(*sock, addr, &packet, sizeof(udp_packet) - 4000);
		if (udp_recv(*sock, addr, &packet, &size, 200) == no_error)
			if (packet.type == syn_ack)
				break;
			else
				--syn_number, --ack_number;
	}

	if (i == 5)								// Если все 5 попыток не удачны, то возвращаем ошибку
		return sock_err("connect (syn)", *sock), connect_error;

	while (1) {								// Отправляем подтверждение на сервер
		packet.type = ack;
		udp_send(*sock, addr, &packet, sizeof(udp_packet) - 4000);
		if (udp_recv(*sock, addr, &packet, &size, 1000) == timeout)	// Проверяем что сервер не отправит повторное сообщение
			break;
	}

	++syn_number;
	++ack_number;

	return no_error;
}

// Отправка запроса на разрыв соединения
errors disconnect_udp_socket(int* sock, sockaddr_in addr) {
	udp_packet packet;
	int i, size;
	// Установка соединения с удаленным хостом 
	for (i = 0; i < 5; ++i) {					// Отпраквка сообщения с подтверждением (аналог функции udp_send_with_ack, но подтверждение "разрыва" имеет тип fin_ack)
		packet.type = fin;
		udp_send(*sock, addr, &packet, sizeof(udp_packet) - 4000);
		if (udp_recv(*sock, addr, &packet, &size, 200) == no_error)
			if (packet.type == fin_ack)
				break;
			else
				--syn_number, --ack_number;
	}

	if (i == 5)									// Если все 5 попыток не удачны, то возвращаем ошибку
		return sock_err("connect (fin)", *sock), connect_error;

	while (1) {									// Отправляем подтверждение на сервер
		packet.type = ack;
		udp_send(*sock, addr, &packet, sizeof(udp_packet) - 4000);
		if (udp_recv(*sock, addr, &packet, &size, 1000) == timeout)	// Проверяем что сервер не отправит повторное сообщение
			break;
	}

	return no_error;
}

// Отправка короткого сообщения
errors send_message(char* argv[], sockaddr_in addr) {
	int sock;
	tcp_packet packet;
	// Установка TCP соединения
	if (connect_tcp_socket(&sock, addr) != no_error)
		return connect_error;
	// Инициализация сообщения (установка типа и копирования текста сообщения)
	packet.type = short_message;
	strncpy(packet.buffer, argv[3], 256);
	// Отправка ообщения
	send(sock, (char*)&packet, sizeof(tcp_packet), 0);

	closesocket(sock);
	return no_error;
}

// Отправка файла на сервер
errors send_file(char* argv[], sockaddr_in addr) {
	int sock;
	int i = 4000, size = 0;
	udp_packet packet, recv_packet;
	errors err = no_error;
	// Установка udp соединения
	if (connect_udp_socket(&sock, addr) != no_error)
		return connect_error;
	// ОТправка информации о файле
	packet.type = send_info_file;
	if (get_file_size(argv[3], &packet.payload.info_file.size) == file_not_found) {
		err = file_not_found;
		fprintf(stderr, "File %s not found\n", argv[3]);
		goto disconnect;
	}

	strncpy(packet.payload.info_file.name, argv[3], 4000 - sizeof(unsigned long long));

	if (udp_send_with_ack(sock, addr, &packet, &size, sizeof(udp_packet)) != no_error) {
		err = send_error;
		goto disconnect;
	}
	// Отправка файла
	FILE* in = fopen(argv[3], "rb");

	if (in == NULL) {
		err = file_not_found;
		goto disconnect;
	}

	while (i == 4000) {
		packet.type = send_part_file;
		i = fread(packet.payload.buffer, 1, 4000, in);
		if (udp_send_with_ack(sock, addr, &packet, &size, sizeof(udp_packet) - 4000 + i) != no_error) {
			err = send_error;
			break;
		}
	};

	fclose(in);

disconnect:
	disconnect_udp_socket(&sock, addr);
	closesocket(sock);
	return err;
}

// Приём файла с сервера
errors recv_file(char* argv[], sockaddr_in addr) {
	int sock;
	int i = 4008, size = 0;
	udp_packet packet, recv_packet;
	errors err = no_error;

	if (connect_udp_socket(&sock, addr) != no_error)
		return connect_error;
	// Отправка запроса на получения файла
	packet.type = recv_info_file;
	strncpy(packet.payload.info_file.name, argv[3], 4000 - sizeof(unsigned long long));

	if (udp_send_with_reply(sock, addr, &packet, &size, &recv_packet, sizeof(udp_packet)) != no_error) {
		err = send_error;
		goto disconnect;
	}
	// Если файл не найден на сервере
	if (recv_packet.type == wrong_file_name) {
		err = file_not_found;
		fprintf(stderr, "File %s not found\n", argv[3]);
		goto disconnect;
	}
	// Приём и запись файла
	FILE* out = fopen(argv[3], "wb");

	if (out == NULL) {
		err = file_not_found;
		goto disconnect;
	}

	while (i == 4008) {
		packet.type = recv_part_file;
		if (udp_send_with_reply(sock, addr, &packet, &i, &recv_packet, sizeof(udp_packet)) != no_error) {
			err = send_error;
			break;
		}
		fwrite(recv_packet.payload.buffer, 1, i - 8, out);
	};
	// Отправка подтверждения о приёме последней части файла
	packet.type = recv_end;
	if (udp_send_with_reply(sock, addr, &packet, &size, &recv_packet, sizeof(udp_packet)) != no_error)
		err = send_error;

	fclose(out);

disconnect:
	disconnect_udp_socket(&sock, addr);
	closesocket(sock);
	return err;
}

// Получения списка файлов
errors recv_list_file(char* argv[], sockaddr_in addr) {
	int sock, i = -1;
	tcp_packet packet;
	FILE *out = fopen("list.txt", "w");

	if (out == NULL)
		return no_error;
	// Установка соединения
	if (connect_tcp_socket(&sock, addr) != no_error)
		return connect_error;
	// Отправка запроса на получения "имени" превого фалйа
	packet.type = recv_file_list;
	send(sock, (char*)&packet, 12, 0);
	recv(sock, (char*)&packet, sizeof(tcp_packet), 0);

	while (packet.type != end_file_list) {	// Отправка запросов для получения оставшихся "имён" файлов
		fprintf(out, "%s\n", packet.buffer);
		packet.type = recv_file_list;
		send(sock, (char*)&packet, 12, 0);
		recv(sock, (char*)&packet, sizeof(tcp_packet), 0);
	};

	fclose(out);

disconnect:
	closesocket(sock);
	return no_error;
}

int main(int argc, char* argv[]) {
	if (argc < 3 || argc > 4) {								// Проверка аргументов
		printf("Error: wrong amount arguments\n");
		return 1;
	}
	char *find_port = 0;

	for (find_port = argv[2]; *find_port != 0 && *find_port != ':'; ++find_port) {
		if ((*find_port < '0' || *find_port > '9') && *find_port != '.')
			return printf("Wrong ip address\n"), 0;
	}

	if (*find_port != ':')
		return printf("Port not found\n"), 0;
	*find_port = 0;
	++find_port;
	for (int i = 0; find_port[i] != 0; ++i)
		if (find_port[i] < '0' || find_port[i] > '9')
			return printf("Wrong port\n"), 0;

	int socket;
	sockaddr_in addr;

	WSADATA wsa_data;
	WSAStartup(MAKEWORD(2, 2), &wsa_data);
	char route[MAX_PATH];
	// Заполнение структуры с адресом удаленного узла  
	addr.sin_family = AF_INET;
	addr.sin_port = htons(atoi(find_port));
	addr.sin_addr.s_addr = inet_addr(argv[2]);
	// Выбор нужного функционала
	if (strcmp(argv[1], "sendmsg") == 0)
		send_message(argv, addr);
	if (strcmp(argv[1], "send") == 0)
		send_file(argv, addr);
	if (strcmp(argv[1], "recv") == 0)
		recv_file(argv, addr);
	if (strcmp(argv[1], "list") == 0)
		recv_list_file(argv, addr);
	return 0;
}
