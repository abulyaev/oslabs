#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>

// ��������� ����������: ������������ ���������� �������   
#pragma comment(lib, "ws2_32.lib") 

#include <stdio.h>

enum errors {					// ������ ��������, ������� ����� ���������� �������
	no_error,						// ��� ��������� ���������� �������
	wrong_ack,						// ��������� ����� �������
	wrong_syn,						// ��������� ����� �������
	wrong_accept,					// ������ ������� accept (����������� tcp �������)
	wrong_tcp_type,					// TCP ��������� � ����������� ����� �������
	wrong_udp_type,					// UDP ��������� � ����������� ����� �������
	stop_message,					// �������������� ��������� stop
	connect_error,					// ������ �����������
	timeout,						// ������ ����� (������������ ��� ������������� ��������� �������� ���������)
	file_not_found,					// ���� � ��������� ������ �� ������
	send_error,						// ������ ��������
	no_client,						// ������������ ��� ������� ���������� �� "�������������������" �������
	dyplicate_message				// ������������ ���� ������ ��� ���������� ���������
};

enum type_tcp_packets {			// ���� TCP ���������
	short_message,					// ��������� � "��������� ����������"
	recv_file_list,					// ��������� � �������� ������ ��������� � �����
	end_file_list,					// ������������ ����� �������� ���������� �������� � �����
	reply_list_file					// ����� �� ��������� � �������� ������ ���������
};

enum type_udp_packets {			// ���� UDP ���������
	syn, syn_ack,					// ����� ��� ������������� ������ (syn - ������ �� ������������� ������ (�� �������), syn_ack - ������������� ������������� ������ (�� �������))
	ack,							// ������������� ��������� (������������ �� ������ �������� ���������)
	send_info_file,					// �������� ���������� � ����� (����� � �������) �� ������
	send_part_file,					// �������� ����� ����� �� ������
	recv_info_file,					// ������ �� �������� ����� � �������
	recv_part_file,					// ������ �� �������� ����� ����� � �������
	recv_end,						// ������ ���� ��������� ����� �������� ���� ������ �����
	wrong_file_name,				// �������� ������� �� ���� � ��������� ������ �� ������
	fin, fin_ack					// ����� ��� ���������� ������ (fin ������������ �������� ��� ���������� ������, fin_ack - ������������� �� �������)
};

struct udp_packet {				// ��������� UDP ������
	unsigned short syn;				// ����� ��������� ������������� �������� � ������ ������
	unsigned short ack;				// ����� ��������� ������������� �������� � ������ ������
	type_udp_packets type;			// ��� ��������� (������ type_udp_packets)
	union {							// ���� �������� ��������
		char  buffer[4000];				// ������ 4000 ����
		struct send_info_file {			// ���������� � �����
			unsigned long long size;
			char name[4000 - sizeof(unsigned long long)];
		} info_file;
	} payload;
};

struct tcp_packet {				// ��������� TCP ������
	type_tcp_packets type;			// ��� ��������� (������ type_tcp_packets)
	char  buffer[4000];				// 4000 ���� �������� ��������
};

unsigned short syn_number, ack_number;	// ���������� ��������� ����������� �������� � �������

										// �������� UDP ������
errors udp_send(int socket, sockaddr_in addr, udp_packet *packet, int len) {
	packet->syn = syn_number;
	packet->ack = ack_number;
	sendto(socket, (char*)packet, len, 0, (sockaddr*)&addr, sizeof(sockaddr_in));
	return no_error;
};

// ���� UDP ������
errors udp_recv(int socket, sockaddr_in addr, udp_packet *packet, int *size, int time_wait) {
	WSAEVENT events;									// ������������� ������� ��� ��������� �������� ���������
	int addrlen = sizeof(sockaddr);

	events = WSACreateEvent();
	WSAEventSelect(socket, events, FD_READ);

	WSANETWORKEVENTS ne;								// �������� ��������� � ������� time_wait
	DWORD dw = WSAWaitForMultipleEvents(1, &events, FALSE, time_wait, FALSE);
	WSAResetEvent(events);
	// ���� ��������� �� ��������� � ������� time_wait
	if (0 != WSAEnumNetworkEvents(socket, events, &ne) || !(ne.lNetworkEvents & FD_READ))
		return timeout;
	// ��������� ���������
	*size = recvfrom(socket, (char*)packet, sizeof(udp_packet), 0, (struct sockaddr*) &addr, &addrlen);
	if (packet->ack != ack_number + 1)					// �������� ������� ���������, ������ ������ ��������� ���������� �������������/������� (ack_number) � �������� ��� ��������� ���������� �������� (syn_number)
		return wrong_ack;
	if (packet->syn != syn_number)
		return wrong_syn;
	++ack_number;
	++syn_number;
	return no_error;
};

// �������� ��������� � �������� ��� ������������
errors udp_send_with_ack(int socket, sockaddr_in addr, udp_packet *packet, int *size, int len) {
	int i;
	udp_packet recv_packet;

	for (i = 0; i < 5; ++i) {
		udp_send(socket, addr, packet, len);			// ���������� ���������
		if (udp_recv(socket, addr, &recv_packet, size, 200) == no_error)	// ���� ������ ����� 
			if (recv_packet.type == ack)										// � �� ����� ��� ������������� (ack)
				break;															// ��������� ��������
	}													// ����� ��������� (�������� 5 ���)

	if (i == 5)											// ���� ��� 5 ������� �� ������, �� ���������� ������
		return send_error;
	return no_error;
}

// �������� ��������� � �������� ������ �� ���� (������ ���������� �������, ������ ����� (recv_packet) �������� ��������)
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

// ������� ��� ��������� ������� �����
errors get_file_size(char *name, unsigned long long* size) {
	HANDLE handl;
	WIN32_FIND_DATA desc;														// ��������� � ��������� ��������� ���������� ������� �������� �������, ��� ����� ������
	handl = FindFirstFile(name, &desc);											// ������� ���� � �������� ������

	if (handl == NULL)
		return file_not_found;
	*size = ((unsigned long long)desc.nFileSizeHigh << 32) + desc.nFileSizeLow;	// ������ 64 ����, �������� � ���� 32 ������� ����� ��������� (� nFileSizeHigh - ������� 32 ����, nFileSizeLow - ������� 32)
	return no_error;
}

// ������������� tcp ����������
errors connect_tcp_socket(int* sock, sockaddr_in addr) {
	*sock = socket(AF_INET, SOCK_STREAM, 0);   // �������� TCP-������  

											   // ��������� ���������� � ��������� ������  
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

	*sock = socket(AF_INET, SOCK_DGRAM, 0);   // �������� UDP-������  

											  // ��������� ���������� � ��������� ������  
	syn_number = 0;							// ���������� ��������� �������� "���������� ��������� ������� ���������"
	ack_number = 0;							// ���������� ��������� �������� "���������� ��������� �������� ���������"

	for (i = 0; i < 5; ++i) {				// ��������� ��������� � �������������� (������ ������� udp_send_with_ack, �� ������������� ������������� ����� ��� syn_ack)
		packet.type = syn;
		udp_send(*sock, addr, &packet, sizeof(udp_packet) - 4000);
		if (udp_recv(*sock, addr, &packet, &size, 200) == no_error)
			if (packet.type == syn_ack)
				break;
			else
				--syn_number, --ack_number;
	}

	if (i == 5)								// ���� ��� 5 ������� �� ������, �� ���������� ������
		return sock_err("connect (syn)", *sock), connect_error;

	while (1) {								// ���������� ������������� �� ������
		packet.type = ack;
		udp_send(*sock, addr, &packet, sizeof(udp_packet) - 4000);
		if (udp_recv(*sock, addr, &packet, &size, 1000) == timeout)	// ��������� ��� ������ �� �������� ��������� ���������
			break;
	}

	++syn_number;
	++ack_number;

	return no_error;
}

// �������� ������� �� ������ ����������
errors disconnect_udp_socket(int* sock, sockaddr_in addr) {
	udp_packet packet;
	int i, size;
	// ��������� ���������� � ��������� ������ 
	for (i = 0; i < 5; ++i) {					// ��������� ��������� � �������������� (������ ������� udp_send_with_ack, �� ������������� "�������" ����� ��� fin_ack)
		packet.type = fin;
		udp_send(*sock, addr, &packet, sizeof(udp_packet) - 4000);
		if (udp_recv(*sock, addr, &packet, &size, 200) == no_error)
			if (packet.type == fin_ack)
				break;
			else
				--syn_number, --ack_number;
	}

	if (i == 5)									// ���� ��� 5 ������� �� ������, �� ���������� ������
		return sock_err("connect (fin)", *sock), connect_error;

	while (1) {									// ���������� ������������� �� ������
		packet.type = ack;
		udp_send(*sock, addr, &packet, sizeof(udp_packet) - 4000);
		if (udp_recv(*sock, addr, &packet, &size, 1000) == timeout)	// ��������� ��� ������ �� �������� ��������� ���������
			break;
	}

	return no_error;
}

// �������� ��������� ���������
errors send_message(char* argv[], sockaddr_in addr) {
	int sock;
	tcp_packet packet;
	// ��������� TCP ����������
	if (connect_tcp_socket(&sock, addr) != no_error)
		return connect_error;
	// ������������� ��������� (��������� ���� � ����������� ������ ���������)
	packet.type = short_message;
	strncpy(packet.buffer, argv[3], 256);
	// �������� ��������
	send(sock, (char*)&packet, sizeof(tcp_packet), 0);

	closesocket(sock);
	return no_error;
}

// �������� ����� �� ������
errors send_file(char* argv[], sockaddr_in addr) {
	int sock;
	int i = 4000, size = 0;
	udp_packet packet, recv_packet;
	errors err = no_error;
	// ��������� udp ����������
	if (connect_udp_socket(&sock, addr) != no_error)
		return connect_error;
	// �������� ���������� � �����
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
	// �������� �����
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

// ���� ����� � �������
errors recv_file(char* argv[], sockaddr_in addr) {
	int sock;
	int i = 4008, size = 0;
	udp_packet packet, recv_packet;
	errors err = no_error;

	if (connect_udp_socket(&sock, addr) != no_error)
		return connect_error;
	// �������� ������� �� ��������� �����
	packet.type = recv_info_file;
	strncpy(packet.payload.info_file.name, argv[3], 4000 - sizeof(unsigned long long));

	if (udp_send_with_reply(sock, addr, &packet, &size, &recv_packet, sizeof(udp_packet)) != no_error) {
		err = send_error;
		goto disconnect;
	}
	// ���� ���� �� ������ �� �������
	if (recv_packet.type == wrong_file_name) {
		err = file_not_found;
		fprintf(stderr, "File %s not found\n", argv[3]);
		goto disconnect;
	}
	// ���� � ������ �����
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
	// �������� ������������� � ����� ��������� ����� �����
	packet.type = recv_end;
	if (udp_send_with_reply(sock, addr, &packet, &size, &recv_packet, sizeof(udp_packet)) != no_error)
		err = send_error;

	fclose(out);

disconnect:
	disconnect_udp_socket(&sock, addr);
	closesocket(sock);
	return err;
}

// ��������� ������ ������
errors recv_list_file(char* argv[], sockaddr_in addr) {
	int sock, i = -1;
	tcp_packet packet;
	FILE *out = fopen("list.txt", "w");

	if (out == NULL)
		return no_error;
	// ��������� ����������
	if (connect_tcp_socket(&sock, addr) != no_error)
		return connect_error;
	// �������� ������� �� ��������� "�����" ������� �����
	packet.type = recv_file_list;
	send(sock, (char*)&packet, 12, 0);
	recv(sock, (char*)&packet, sizeof(tcp_packet), 0);

	while (packet.type != end_file_list) {	// �������� �������� ��� ��������� ���������� "���" ������
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
	if (argc < 3 || argc > 4) {								// �������� ����������
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
	// ���������� ��������� � ������� ���������� ����  
	addr.sin_family = AF_INET;
	addr.sin_port = htons(atoi(find_port));
	addr.sin_addr.s_addr = inet_addr(argv[2]);
	// ����� ������� �����������
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
