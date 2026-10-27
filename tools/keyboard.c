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
 * Keys are matched by their position (scan code), so every keyboard layout
 * uses the same physical keys. The key names here are the US layout names.
 *
 * Most PC keyboards report only 6 normal keys at once, so space with all six
 * dots drops a key. Alt is a modifier and does not count toward that limit.
 *
 * With --qt, the window is the Apex QT QWERTY keyboard instead. PC keys go to
 * the same QT keys. The QT keys that a PC does not have are on left Alt
 * (READ), right Alt (FUNCTION), F1 (HELP), F2 (REPEAT), F3 (MENU), and `
 * (the bottom row single quote). Escape is the QT ESCAPE key, so Alt+F4 quits.
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
#define RELEASE_FLAG 0x80
#define EXTENDED_KEY_FLAG (1 << 24)
#define EXTENDED 0x100
#define KEY_COUNT 0x200
#define SCAN_ESCAPE 0x01
#define SCAN_BACK 0x0e
#define SCAN_TAB 0x0f
#define SCAN_RETURN 0x1c
#define SCAN_CONTROL 0x1d
#define SCAN_LSHIFT 0x2a
#define SCAN_RSHIFT 0x36
#define SCAN_ALT 0x38
#define SCAN_SPACE 0x39
#define SCAN_F1 0x3b
#define SCAN_F2 0x3c
#define SCAN_F3 0x3d
#define SCAN_F12 0x58
#define SCAN_UP (EXTENDED | 0x48)
#define SCAN_LEFT (EXTENDED | 0x4b)
#define SCAN_RIGHT (EXTENDED | 0x4d)
#define SCAN_DOWN (EXTENDED | 0x50)
#define SCAN_DELETE (EXTENDED | 0x53)
#define SCAN_NUMPAD_ENTER (EXTENDED | SCAN_RETURN)
#define SCAN_RCONTROL (EXTENDED | SCAN_CONTROL)
#define SCAN_RALT (EXTENDED | SCAN_ALT)
#define POWER_SWITCH_KEY SCAN_F12
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
	"Type as on the Apex QT. Left Alt is READ, right Alt is FUNCTION, F1 is HELP, F2 is REPEAT, F3 is MENU, "
	"and ` is the bottom row single quote.\r\n"
	"F12 turns the power switch off or on again. Switch off before you close the emulator.\r\n"
	"Press Alt+F4 to quit.";

/* A key is its scan code, plus EXTENDED for the extended keys. */
typedef struct KeyMapping {
	WORD key;
	unsigned char code;
} KeyMapping;

/* Key numbers from the keypad driver's braille key table. */
static const KeyMapping braille_map[] = {
	{ 0x21, 25 },               /* F, dot 1 */
	{ 0x20, 26 },               /* D, dot 2 */
	{ 0x1f, 27 },               /* S, dot 3 */
	{ 0x24, 22 },               /* J, dot 4 */
	{ 0x25, 21 },               /* K, dot 5 */
	{ 0x26, 20 },               /* L, dot 6 */
	{ 0x1e, 28 },               /* A, dot 7 */
	{ SCAN_BACK, 28 },          /* dot 7 */
	{ 0x27, 19 },               /* ;, dot 8 */
	{ SCAN_RETURN, 19 },        /* dot 8 */
	{ SCAN_NUMPAD_ENTER, 19 },  /* dot 8 */
	{ SCAN_SPACE, 125 },        /* space */
	{ SCAN_ALT, 125 },          /* space, left Alt */
	{ SCAN_RALT, 125 },         /* space, right Alt */
};

/* Key numbers from the keypad driver's QT key table (US English). */
static const KeyMapping qt_map[] = {
	{ 0x1e, 28 }, /* A */ { 0x30, 73 }, /* B */ { 0x2e, 75 }, /* C */ { 0x20, 26 }, /* D */
	{ 0x12, 43 }, /* E */ { 0x21, 25 }, /* F */ { 0x22, 24 }, /* G */ { 0x23, 23 }, /* H */
	{ 0x17, 38 }, /* I */ { 0x24, 22 }, /* J */ { 0x25, 21 }, /* K */ { 0x26, 20 }, /* L */
	{ 0x32, 71 }, /* M */ { 0x31, 72 }, /* N */ { 0x18, 37 }, /* O */ { 0x19, 36 }, /* P */
	{ 0x10, 45 }, /* Q */ { 0x13, 42 }, /* R */ { 0x1f, 27 }, /* S */ { 0x14, 41 }, /* T */
	{ 0x16, 39 }, /* U */ { 0x2f, 74 }, /* V */ { 0x11, 44 }, /* W */ { 0x2d, 76 }, /* X */
	{ 0x15, 40 }, /* Y */ { 0x2c, 77 }, /* Z */
	{ 0x02, 61 }, /* 1 */ { 0x03, 60 }, /* 2 */ { 0x04, 59 }, /* 3 */ { 0x05, 58 }, /* 4 */
	{ 0x06, 57 }, /* 5 */ { 0x07, 56 }, /* 6 */ { 0x08, 55 }, /* 7 */ { 0x09, 54 }, /* 8 */
	{ 0x0a, 53 }, /* 9 */ { 0x0b, 52 }, /* 0 */
	{ 0x0c, 51 },              /* - */
	{ 0x0d, 35 },              /* = */
	{ 0x1a, 87 },              /* [ */
	{ 0x1b, 86 },              /* ] */
	{ 0x2b, 85 },              /* backslash */
	{ 0x27, 19 },              /* ; */
	{ 0x28, 84 },              /* ' */
	{ 0x33, 70 },              /* , */
	{ 0x34, 69 },              /* . */
	{ 0x35, 68 },              /* / */
	{ 0x29, 120 },             /* `, the bottom row single quote */
	{ SCAN_ESCAPE, 62 },
	{ SCAN_TAB, 122 },
	{ SCAN_RETURN, 83 },
	{ SCAN_NUMPAD_ENTER, 83 },
	{ SCAN_BACK, 99 },
	{ SCAN_DELETE, 119 },
	{ SCAN_SPACE, 125 },
	{ SCAN_UP, 100 },
	{ SCAN_RIGHT, 101 },
	{ SCAN_DOWN, 102 },
	{ SCAN_LEFT, 103 },
	{ SCAN_LSHIFT, 30 },
	{ SCAN_RSHIFT, 46 },
	{ SCAN_CONTROL, 14 },
	{ SCAN_RCONTROL, 14 },
	{ SCAN_ALT, 116 },         /* READ, left Alt */
	{ SCAN_RALT, 67 },         /* FUNCTION, right Alt */
	{ SCAN_F1, 126 },          /* HELP */
	{ SCAN_F2, 78 },           /* REPEAT */
	{ SCAN_F3, 93 },           /* MENU */
};

static const KeyMapping *key_map = braille_map;
static size_t key_count = sizeof(braille_map) / sizeof(braille_map[0]);
static BOOL qt;

static SOCKET sock = INVALID_SOCKET;
static BOOL held[KEY_COUNT];
static BOOL power_off;
static HWND label;

static int find_code(WORD key)
{
	size_t i;

	for (i = 0; i < key_count; i++) {
		if (key_map[i].key == key) {
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

static void handle_key(HWND window, WORD key, BOOL down)
{
	int code;
	int sent;

	if (key == SCAN_ESCAPE && !qt) {
		DestroyWindow(window);
		return;
	}
	code = find_code(key);
	if (code < 0 && key != POWER_SWITCH_KEY) {
		return;
	}
	/* Held keys repeat their key-down event; send only the first one. */
	if (down == held[key]) {
		return;
	}
	held[key] = down;
	if (key == POWER_SWITCH_KEY) {
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
		if (held[key_map[i].key]) {
			handle_key(window, key_map[i].key, FALSE);
		}
	}
}

/* Programs that send keys, such as some screen readers, can leave out the scan code. */
static WORD key_from_message(WPARAM virtual_key, LPARAM lparam)
{
	UINT scan_code = (lparam >> 16) & 0xff;

	if (scan_code == 0) {
		scan_code = MapVirtualKeyA((UINT)virtual_key, MAPVK_VK_TO_VSC_EX);
		return (scan_code & 0xff) | ((scan_code & 0xff00) ? EXTENDED : 0);
	}
	return scan_code | ((lparam & EXTENDED_KEY_FLAG) ? EXTENDED : 0);
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
		if (find_code(key_from_message(wparam, lparam)) < 0) {
			break;
		}
		/* fall through */
	case WM_KEYDOWN:
	case WM_KEYUP:
		handle_key(window, key_from_message(wparam, lparam), message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
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
