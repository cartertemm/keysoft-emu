/*
 * Braille keyboard for the emulated BrailleNote Apex.
 *
 * Reads key presses and releases from this console window and sends them to
 * the key controller of the running emulator over TCP. Chords work because
 * every key is sent down and up separately, like the real keyboard does.
 *
 * Keys: F D S = dots 1 2 3, J K L = dots 4 5 6, Space = space,
 * Backspace = dot 7, Enter = dot 8. Escape quits.
 *
 * Build: gcc -O2 -o keyboard.exe tools/keyboard.c -lws2_32
 * Usage: keyboard.exe [port]   (default port 5555)
 */

#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <windows.h>

#define DEFAULT_PORT 5555
#define BRAILLE_FIFO 1
#define RELEASE_FLAG 0x80
#define KEY_COUNT 256

typedef struct KeyMapping {
	WORD virtual_key;
	unsigned char code;
} KeyMapping;

/* Key numbers from the keypad driver's braille key table. */
static const KeyMapping key_map[] = {
	{ 'F', 25 },         /* dot 1 */
	{ 'D', 26 },         /* dot 2 */
	{ 'S', 27 },         /* dot 3 */
	{ 'J', 22 },         /* dot 4 */
	{ 'K', 21 },         /* dot 5 */
	{ 'L', 20 },         /* dot 6 */
	{ VK_BACK, 28 },     /* dot 7 */
	{ VK_RETURN, 19 },   /* dot 8 */
	{ VK_SPACE, 125 },   /* space */
};

static int find_code(WORD virtual_key)
{
	size_t i;

	for (i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
		if (key_map[i].virtual_key == virtual_key) {
			return key_map[i].code;
		}
	}
	return -1;
}

static SOCKET connect_to_emulator(int port)
{
	struct sockaddr_in address = { 0 };
	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (sock == INVALID_SOCKET) {
		return INVALID_SOCKET;
	}
	address.sin_family = AF_INET;
	address.sin_port = htons((u_short)port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(sock, (struct sockaddr *)&address, sizeof(address)) != 0) {
		closesocket(sock);
		return INVALID_SOCKET;
	}
	return sock;
}

static int send_event(SOCKET sock, int code, int released)
{
	char message[2] = { BRAILLE_FIFO, (char)(code | (released ? RELEASE_FLAG : 0)) };

	return send(sock, message, sizeof(message), 0) == sizeof(message);
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	BOOL held[KEY_COUNT] = { 0 };
	WSADATA wsa;
	SOCKET sock;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "Could not start Windows networking.\n");
		return 1;
	}
	sock = connect_to_emulator(port);
	if (sock == INVALID_SOCKET) {
		fprintf(stderr, "Could not connect to the emulator on port %d. Is it running?\n", port);
		WSACleanup();
		return 1;
	}
	SetConsoleMode(input, ENABLE_WINDOW_INPUT);
	printf("Connected. F D S J K L are dots 1 to 6, Space is space, Backspace is dot 7, Enter is dot 8.\n");
	printf("Press Escape to quit.\n");

	for (;;) {
		INPUT_RECORD record;
		DWORD count;
		KEY_EVENT_RECORD *key;
		int code;

		if (!ReadConsoleInput(input, &record, 1, &count) || count == 0) {
			break;
		}
		if (record.EventType != KEY_EVENT) {
			continue;
		}
		key = &record.Event.KeyEvent;
		if (key->wVirtualKeyCode == VK_ESCAPE) {
			break;
		}
		code = find_code(key->wVirtualKeyCode);
		if (code < 0 || key->wVirtualKeyCode >= KEY_COUNT) {
			continue;
		}
		/* Held keys repeat their key-down event; send only the first one. */
		if (key->bKeyDown == held[key->wVirtualKeyCode]) {
			continue;
		}
		held[key->wVirtualKeyCode] = key->bKeyDown;
		if (!send_event(sock, code, !key->bKeyDown)) {
			fprintf(stderr, "The emulator closed the connection.\n");
			break;
		}
	}
	closesocket(sock);
	WSACleanup();
	return 0;
}
