/*
 * Braille or QWERTY keyboard for the emulated BrailleNote Apex.
 *
 * Reads key presses and releases from its window and sends them to the key
 * controller of the running emulator over TCP. Chords work because every key
 * is sent down and up separately, like the real keyboard does.
 *
 * Keys: F D S = dots 1 2 3, J K L = dots 4 5 6, Space or Alt = space,
 * A or Backspace = dot 7, ; or Enter = dot 8. F12 flips the power switch off and on.
 * Escape quits.
 *
 * Most PC keyboards report only 6 normal keys at once, so space with all six
 * dots drops a key. Alt is a modifier and does not count toward that limit.
 *
 * With --qt, the window is the Apex QT QWERTY keyboard instead. PC keys go to
 * the same QT keys. The QT keys that a PC does not have are on Caps Lock
 * (READ), Alt (FUNCTION), F1 (HELP), F2 (REPEAT), F3 (MENU), and ` (the bottom
 * row single quote). Escape is the QT ESCAPE key, so Alt+F4 quits.
 *
 * Build: gcc -O2 -mwindows -o keyboard.exe tools/keyboard.c -lws2_32
 * Usage: keyboard.exe [--qt] [port]   (default port 23571)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#define DEFAULT_PORT 23571
#define KEYBOARD_FIFO 1
#define POWER_SWITCH_SOURCE 3
#define POWER_SWITCH_KEY VK_F12
#define RELEASE_FLAG 0x80
#define KEY_COUNT 256
#define WINDOW_WIDTH 480
#define WINDOW_HEIGHT 160
#define INSTANCE_MUTEX "Local\\keysoft-emu-keyboard"

static const char window_class[] = "BrailleKeyboard";
static const char braille_title[] = "Braille Keyboard";
static const char braille_instructions[] =
	"F D S J K L are dots 1 to 6, Space or Alt is space, A or Backspace is dot 7, ; or Enter is dot 8.\r\n"
	"F12 turns the power switch off or on again. Switch off before you close the emulator.\r\n"
	"Press Escape to quit.";
static const char qt_title[] = "QWERTY Keyboard";
static const char qt_instructions[] =
	"Type as on the Apex QT. Caps Lock is READ, Alt is FUNCTION, F1 is HELP, F2 is REPEAT, F3 is MENU, "
	"and ` is the bottom row single quote.\r\n"
	"F12 turns the power switch off or on again. Switch off before you close the emulator.\r\n"
	"Press Alt+F4 to quit.";

typedef struct KeyMapping {
	WORD virtual_key;
	unsigned char code;
} KeyMapping;

/* Key numbers from the keypad driver's braille key table. */
static const KeyMapping braille_map[] = {
	{ 'F', 25 },         /* dot 1 */
	{ 'D', 26 },         /* dot 2 */
	{ 'S', 27 },         /* dot 3 */
	{ 'J', 22 },         /* dot 4 */
	{ 'K', 21 },         /* dot 5 */
	{ 'L', 20 },         /* dot 6 */
	{ 'A', 28 },         /* dot 7 */
	{ VK_BACK, 28 },     /* dot 7 */
	{ VK_OEM_1, 19 },    /* dot 8, the ; key */
	{ VK_RETURN, 19 },   /* dot 8 */
	{ VK_SPACE, 125 },   /* space */
	{ VK_MENU, 125 },    /* space, either Alt key */
};

/* Key numbers from the keypad driver's QT key table (US English). */
static const KeyMapping qt_map[] = {
	{ 'A', 28 }, { 'B', 73 }, { 'C', 75 }, { 'D', 26 }, { 'E', 43 }, { 'F', 25 }, { 'G', 24 },
	{ 'H', 23 }, { 'I', 38 }, { 'J', 22 }, { 'K', 21 }, { 'L', 20 }, { 'M', 71 }, { 'N', 72 },
	{ 'O', 37 }, { 'P', 36 }, { 'Q', 45 }, { 'R', 42 }, { 'S', 27 }, { 'T', 41 }, { 'U', 39 },
	{ 'V', 74 }, { 'W', 44 }, { 'X', 76 }, { 'Y', 40 }, { 'Z', 77 },
	{ '1', 61 }, { '2', 60 }, { '3', 59 }, { '4', 58 }, { '5', 57 },
	{ '6', 56 }, { '7', 55 }, { '8', 54 }, { '9', 53 }, { '0', 52 },
	{ VK_OEM_MINUS, 51 },
	{ VK_OEM_PLUS, 35 },   /* = */
	{ VK_OEM_4, 87 },      /* [ */
	{ VK_OEM_6, 86 },      /* ] */
	{ VK_OEM_5, 85 },      /* backslash */
	{ VK_OEM_1, 19 },      /* ; */
	{ VK_OEM_7, 84 },      /* ' */
	{ VK_OEM_COMMA, 70 },
	{ VK_OEM_PERIOD, 69 },
	{ VK_OEM_2, 68 },      /* / */
	{ VK_OEM_3, 120 },     /* `, the bottom row single quote */
	{ VK_ESCAPE, 62 },
	{ VK_TAB, 122 },
	{ VK_RETURN, 83 },
	{ VK_BACK, 99 },
	{ VK_DELETE, 119 },
	{ VK_SPACE, 125 },
	{ VK_UP, 100 },
	{ VK_RIGHT, 101 },
	{ VK_DOWN, 102 },
	{ VK_LEFT, 103 },
	{ VK_LSHIFT, 30 },
	{ VK_RSHIFT, 46 },
	{ VK_CONTROL, 14 },    /* either Ctrl key */
	{ VK_CAPITAL, 116 },   /* READ */
	{ VK_MENU, 67 },       /* FUNCTION, either Alt key */
	{ VK_F1, 126 },        /* HELP */
	{ VK_F2, 78 },         /* REPEAT */
	{ VK_F3, 93 },         /* MENU */
};

static const KeyMapping *key_map = braille_map;
static size_t key_count = sizeof(braille_map) / sizeof(braille_map[0]);
static BOOL qt;

static SOCKET sock = INVALID_SOCKET;
static BOOL held[KEY_COUNT];
static BOOL power_off;
static HWND label;

static int find_code(WPARAM virtual_key)
{
	size_t i;

	for (i = 0; i < key_count; i++) {
		if (key_map[i].virtual_key == virtual_key) {
			return key_map[i].code;
		}
	}
	return -1;
}

static SOCKET connect_to_emulator(int port)
{
	struct sockaddr_in address = { 0 };
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (s == INVALID_SOCKET) {
		return INVALID_SOCKET;
	}
	address.sin_family = AF_INET;
	address.sin_port = htons((u_short)port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, (struct sockaddr *)&address, sizeof(address)) != 0) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

static int send_message(int source, int entry)
{
	char message[2] = { (char)source, (char)entry };

	return send(sock, message, sizeof(message), 0) == sizeof(message);
}

static int send_event(int code, int released)
{
	return send_message(KEYBOARD_FIFO, code | (released ? RELEASE_FLAG : 0));
}

static const char *window_title(void)
{
	return qt ? qt_title : braille_title;
}

static void show_error(HWND owner, const char *text)
{
	MessageBoxA(owner, text, window_title(), MB_OK | MB_ICONERROR);
}

static void handle_key(HWND window, WPARAM virtual_key, BOOL down)
{
	int code;
	int sent;

	if (virtual_key == VK_ESCAPE && !qt) {
		DestroyWindow(window);
		return;
	}
	code = find_code(virtual_key);
	if ((code < 0 && virtual_key != POWER_SWITCH_KEY) || virtual_key >= KEY_COUNT) {
		return;
	}
	/* Held keys repeat their key-down event; send only the first one. */
	if (down == held[virtual_key]) {
		return;
	}
	held[virtual_key] = down;
	if (virtual_key == POWER_SWITCH_KEY) {
		if (!down) {
			return;
		}
		power_off = !power_off;
		sent = send_message(POWER_SWITCH_SOURCE, power_off);
	} else {
		sent = send_event(code, !down);
	}
	if (!sent) {
		show_error(window, "The emulator closed the connection.");
		DestroyWindow(window);
	}
}

/* Key-up events go to the new focus window, so release held keys ourselves. */
static void release_held_keys(HWND window)
{
	size_t i;

	for (i = 0; i < key_count; i++) {
		if (held[key_map[i].virtual_key]) {
			handle_key(window, key_map[i].virtual_key, FALSE);
		}
	}
}

/* Windows reports both Shift keys as VK_SHIFT, but the QT has a left and a right SHIFT. */
static WPARAM side_of_shift(WPARAM virtual_key, LPARAM lparam)
{
	if (!qt || virtual_key != VK_SHIFT) {
		return virtual_key;
	}
	return MapVirtualKeyA((lparam >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_CREATE:
		label = CreateWindowA("STATIC", qt ? qt_instructions : braille_instructions, WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, window, NULL, ((CREATESTRUCT *)lparam)->hInstance, NULL);
		SendMessageA(label, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), FALSE);
		return 0;
	case WM_SIZE:
		MoveWindow(label, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
		return 0;
	/* Keys pressed while Alt is down come as system keys. */
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
		if (find_code(side_of_shift(wparam, lparam)) < 0) {
			break;
		}
		/* fall through */
	case WM_KEYDOWN:
	case WM_KEYUP:
		handle_key(window, side_of_shift(wparam, lparam), message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
		return 0;
	case WM_KILLFOCUS:
		release_held_keys(window);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(window, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show)
{
	int port = DEFAULT_PORT;
	char error[128];
	WNDCLASSA wc = { 0 };
	WSADATA wsa;
	HWND window;
	MSG msg;
	int i;

	(void)previous;
	(void)command_line;
	for (i = 1; i < __argc; i++) {
		if (!strcmp(__argv[i], "--qt")) {
			qt = TRUE;
			key_map = qt_map;
			key_count = sizeof(qt_map) / sizeof(qt_map[0]);
		} else {
			port = atoi(__argv[i]);
		}
	}
	if (CreateMutexA(NULL, TRUE, INSTANCE_MUTEX) && GetLastError() == ERROR_ALREADY_EXISTS) {
		window = FindWindowA(window_class, NULL);
		if (window) {
			SetForegroundWindow(window);
		}
		return 0;
	}
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		show_error(NULL, "Could not start Windows networking.");
		return 1;
	}
	sock = connect_to_emulator(port);
	if (sock == INVALID_SOCKET) {
		snprintf(error, sizeof(error), "Could not connect to the emulator on port %d. Is it running?", port);
		show_error(NULL, error);
		WSACleanup();
		return 1;
	}

	wc.lpfnWndProc = window_proc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = window_class;
	RegisterClassA(&wc);
	window = CreateWindowA(window_class, window_title(), WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH, WINDOW_HEIGHT, NULL, NULL, instance, NULL);
	ShowWindow(window, show);

	while (GetMessageA(&msg, NULL, 0, 0) > 0) {
		DispatchMessageA(&msg);
	}
	closesocket(sock);
	WSACleanup();
	return 0;
}
