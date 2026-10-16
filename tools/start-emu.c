/*
 * Launcher for the portable emulator.
 *
 * On the first run it extracts the firmware from the KeySoft .hwi file in its
 * folder and checks it. Then it creates the NAND images and starts QEMU and
 * the braille keyboard. The QEMU arguments match scripts/run.sh.
 *
 * Build: gcc -O2 -DVERSION=\"$(cat VERSION)\" -o start-emu.exe tools/start-emu.c -lws2_32
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define BEX_MAGIC "HUMACE60"
#define BEX_MAGIC_SIZE 8
#define BEX_HEADER_SIZE 0x200
#define BEX_NAME_OFFSET 0x20
#define ROM_SIGNATURE "ECEC"
#define ROM_SIGNATURE_SIZE 4
#define ROM_SIGNATURE_OFFSET 0x40
#define ROM_TOC_POINTER_OFFSET 0x48
#define ROMHDR_SIZE 84
#define ROMHDR_PHYSFIRST 2
#define ROMHDR_MODULE_COUNT 4
#define MODULE_ENTRY_SIZE 32
#define MODULE_ENTRY_NAME 4
#define KERNEL_ADDR 0x80200000u
#define BOOT_STUB_ADDR 0x80010000u
#define FPGA_NOR_ADDR 0xa00c0000u
#define FPGA_FILE_HEADER_BYTES 0x48u
#define NAND_CHIP_COUNT 4
#define NAND_IMAGE_BYTES "2264924160"
#define TELNET_PORT 2323
#define KEYS_PORT 23571
#define KEYS_PORT_TRIES 10
#define KEYS_WAIT_MS 30000
#define KEYS_POLL_MS 250
#define COMMAND_SIZE 4096
#define PATH_SIZE (2 * MAX_PATH)
#define INSTANCE_MUTEX "Local\\keysoft-emu-start-emu"
#define IMAGE_NAME_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"

static const char kernel_image[] = "extracted\\Everest_Kernel\\image.bin";
static const char fpga_image[] = "extracted\\Everest_Fpga\\image.bin";
static const char required_module[] = "KeySoft.exe";

/* The instructions that qemu/hw/arm/everest.c patches, with their original values. */
static const struct {
	unsigned int addr;
	unsigned int original;
} patched_words[] = {
	{ 0x81ddc534, 0xe1a00004 },
	{ 0x813aee14, 0xebfffc58 },
	{ 0x813af370, 0xebfffb01 },
	{ 0x8239dc0c, 0x1a000003 },
};

typedef struct Buffer {
	unsigned char *data;
	size_t size;
} Buffer;

static void delete_tree(const char *path)
{
	char pattern[MAX_PATH];
	char child[PATH_SIZE];
	WIN32_FIND_DATAA found;
	HANDLE search;

	snprintf(pattern, sizeof(pattern), "%s\\*", path);
	search = FindFirstFileA(pattern, &found);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) {
				continue;
			}
			snprintf(child, sizeof(child), "%s\\%s", path, found.cFileName);
			if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				delete_tree(child);
			} else {
				SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
				DeleteFileA(child);
			}
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	RemoveDirectoryA(path);
}

static void fail(BOOL clean, const char *format, ...)
{
	va_list arguments;

	if (clean) {
		delete_tree("firmware");
		delete_tree("extracted");
	}
	fputs("Error: ", stdout);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\nPress Enter to close.\n");
	getchar();
	exit(1);
}

static BOOL read_file(const char *path, Buffer *buffer)
{
	FILE *file = fopen(path, "rb");
	long size;
	BOOL ok;

	if (!file) {
		return FALSE;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	buffer->size = size;
	buffer->data = malloc(size ? size : 1);
	ok = buffer->data && fread(buffer->data, 1, size, file) == (size_t)size;
	fclose(file);
	if (!ok) {
		free(buffer->data);
	}
	return ok;
}

static BOOL write_file(const char *path, const void *data, size_t size)
{
	FILE *file = fopen(path, "wb");
	BOOL ok;

	if (!file) {
		return FALSE;
	}
	ok = fwrite(data, 1, size, file) == size;
	return fclose(file) == 0 && ok;
}

static BOOL file_exists(const char *path)
{
	DWORD attributes = GetFileAttributesA(path);

	return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned int read_u32(const unsigned char *bytes)
{
	return bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (unsigned int)bytes[3] << 24;
}

static BOOL start_program(char *command, BOOL quiet, PROCESS_INFORMATION *process)
{
	STARTUPINFOA startup = { 0 };
	SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
	HANDLE nul = INVALID_HANDLE_VALUE;
	BOOL started;

	startup.cb = sizeof(startup);
	if (quiet) {
		nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = nul;
		startup.hStdError = nul;
	}
	started = CreateProcessA(NULL, command, NULL, NULL, quiet, 0, NULL, NULL, &startup, process);
	if (nul != INVALID_HANDLE_VALUE) {
		CloseHandle(nul);
	}
	return started;
}

static BOOL run_program(char *command, BOOL quiet, DWORD *exit_code)
{
	PROCESS_INFORMATION process;

	if (!start_program(command, quiet, &process)) {
		return FALSE;
	}
	WaitForSingleObject(process.hProcess, INFINITE);
	GetExitCodeProcess(process.hProcess, exit_code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return TRUE;
}

static const char *check_kernel(const Buffer *kernel)
{
	static char message[128];
	unsigned int toc;
	unsigned int base;
	unsigned int module_count;
	unsigned int i;
	BOOL found = FALSE;

	if (kernel->size < ROM_TOC_POINTER_OFFSET + 4 ||
		memcmp(kernel->data + ROM_SIGNATURE_OFFSET, ROM_SIGNATURE, ROM_SIGNATURE_SIZE) != 0) {
		return "The kernel image has no Windows CE ROM signature.";
	}
	toc = read_u32(kernel->data + ROM_TOC_POINTER_OFFSET);
	if (kernel->size < ROMHDR_SIZE || toc > kernel->size - ROMHDR_SIZE) {
		return "The kernel ROM table of contents is outside the image.";
	}
	base = read_u32(kernel->data + toc + 4 * ROMHDR_PHYSFIRST);
	module_count = read_u32(kernel->data + toc + 4 * ROMHDR_MODULE_COUNT);
	if (module_count > (kernel->size - toc - ROMHDR_SIZE) / MODULE_ENTRY_SIZE) {
		return "The kernel module list is outside the image.";
	}
	for (i = 0; i < module_count && !found; i++) {
		unsigned int address = read_u32(kernel->data + toc + ROMHDR_SIZE + i * MODULE_ENTRY_SIZE + 4 * MODULE_ENTRY_NAME);
		size_t offset = address - base;

		found = address >= base && offset <= kernel->size - sizeof(required_module) &&
			memcmp(kernel->data + offset, required_module, sizeof(required_module)) == 0;
	}
	if (!found) {
		return "The kernel does not contain KeySoft.exe.";
	}
	for (i = 0; i < sizeof(patched_words) / sizeof(patched_words[0]); i++) {
		size_t offset = patched_words[i].addr - KERNEL_ADDR;

		if (offset > kernel->size - 4 || read_u32(kernel->data + offset) != patched_words[i].original) {
			snprintf(message, sizeof(message),
				"This KeySoft version is not supported. The instruction at 0x%08x does not match.", patched_words[i].addr);
			return message;
		}
	}
	return NULL;
}

static const char *check_firmware(void)
{
	Buffer kernel;
	const char *problem;

	if (!file_exists(fpga_image)) {
		return "The update has no FPGA image (Everest_Fpga).";
	}
	if (!read_file(kernel_image, &kernel)) {
		return "The update has no kernel image (Everest_Kernel).";
	}
	problem = check_kernel(&kernel);
	free(kernel.data);
	return problem;
}

static void find_hwi(char *name, size_t size)
{
	char folder[MAX_PATH];
	char list[COMMAND_SIZE] = "";
	WIN32_FIND_DATAA found;
	HANDLE search = FindFirstFileA("*.hwi", &found);
	int count = 0;

	if (search != INVALID_HANDLE_VALUE) {
		do {
			if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				continue;
			}
			count++;
			snprintf(name, size, "%s", found.cFileName);
			strncat(list, "  ", sizeof(list) - strlen(list) - 1);
			strncat(list, found.cFileName, sizeof(list) - strlen(list) - 1);
			strncat(list, "\n", sizeof(list) - strlen(list) - 1);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	if (count == 0) {
		GetCurrentDirectoryA(sizeof(folder), folder);
		fail(FALSE, "Put your KeySoft .hwi file in this folder: %s", folder);
	}
	if (count > 1) {
		fail(FALSE, "There is more than one .hwi file in this folder. Keep only one:\n%s", list);
	}
}

static int extract_image(const char *path)
{
	char folder[MAX_PATH];
	char image[PATH_SIZE];
	char *name;
	char *extension;
	Buffer data;
	int extracted = 0;

	if (!read_file(path, &data)) {
		return 0;
	}
	if (data.size > BEX_HEADER_SIZE && memcmp(data.data, BEX_MAGIC, BEX_MAGIC_SIZE) == 0 &&
		memchr(data.data + BEX_NAME_OFFSET, 0, BEX_HEADER_SIZE - BEX_NAME_OFFSET)) {
		name = (char *)data.data + BEX_NAME_OFFSET;
		extension = strrchr(name, '.');
		if (extension) {
			*extension = 0;
		}
		if (*name && strspn(name, IMAGE_NAME_CHARS) == strlen(name)) {
			snprintf(folder, sizeof(folder), "extracted\\%s", name);
			snprintf(image, sizeof(image), "%s\\image.bin", folder);
			CreateDirectoryA("extracted", NULL);
			CreateDirectoryA(folder, NULL);
			if (!write_file(image, data.data + BEX_HEADER_SIZE, data.size - BEX_HEADER_SIZE)) {
				fail(TRUE, "Could not write %s.", image);
			}
			extracted = 1;
		}
	}
	free(data.data);
	return extracted;
}

static void extract_firmware(const char *hwi)
{
	char command[COMMAND_SIZE];
	char path[PATH_SIZE];
	WIN32_FIND_DATAA found;
	HANDLE search;
	DWORD exit_code;
	int images = 0;

	printf("Extracting the firmware from %s...\n", hwi);
	delete_tree("firmware");
	delete_tree("extracted");
	CreateDirectoryA("firmware", NULL);
	snprintf(command, sizeof(command), "\"%s\\System32\\expand.exe\" \"%s\" -F:* firmware", getenv("SystemRoot"), hwi);
	if (!run_program(command, TRUE, &exit_code) || exit_code != 0) {
		fail(TRUE, "Windows could not unpack %s. The file may be damaged.", hwi);
	}
	search = FindFirstFileA("firmware\\*", &found);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				snprintf(path, sizeof(path), "firmware\\%s", found.cFileName);
				images += extract_image(path);
			}
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	if (images == 0) {
		fail(TRUE, "%s has no firmware images. The file may be damaged.", hwi);
	}
}

static void create_nand_images(void)
{
	char command[COMMAND_SIZE];
	char image[PATH_SIZE];
	DWORD exit_code;
	int chip;

	CreateDirectoryA("state", NULL);
	for (chip = 0; chip < NAND_CHIP_COUNT; chip++) {
		snprintf(image, sizeof(image), "state\\nand%d.qcow2", chip);
		if (file_exists(image)) {
			continue;
		}
		snprintf(command, sizeof(command), "qemu\\qemu-img.exe create -q -f qcow2 %s " NAND_IMAGE_BYTES, image);
		if (!run_program(command, FALSE, &exit_code)) {
			fail(FALSE, "Could not start qemu\\qemu-img.exe.");
		}
		if (exit_code != 0) {
			fail(FALSE, "qemu\\qemu-img.exe could not create %s.", image);
		}
	}
}

static void start_emulator(PROCESS_INFORMATION *process, int keys_port)
{
	char command[COMMAND_SIZE];
	int length;
	int chip;

	length = snprintf(command, sizeof(command),
		"qemu\\qemu-system-arm.exe -M everest -display none -serial stdio -icount shift=auto"
		" -nic user,model=lan9118,hostfwd=tcp:127.0.0.1:%d-:23"
		" -chardev socket,id=keys,host=127.0.0.1,port=%d,server=on,wait=off",
		TELNET_PORT, keys_port);
	for (chip = 0; chip < NAND_CHIP_COUNT; chip++) {
		length += snprintf(command + length, sizeof(command) - length,
			" -drive if=mtd,index=%d,format=qcow2,file=state\\nand%d.qcow2", chip, chip);
	}
	snprintf(command + length, sizeof(command) - length,
		" -device loader,file=%s,addr=0x%x,force-raw=on"
		" -device loader,file=%s,addr=0x%x,force-raw=on"
		" -device loader,addr=0x%x,cpu-num=0",
		kernel_image, KERNEL_ADDR, fpga_image, FPGA_NOR_ADDR - FPGA_FILE_HEADER_BYTES, BOOT_STUB_ADDR);
	if (!start_program(command, FALSE, process)) {
		fail(FALSE, "Could not start qemu\\qemu-system-arm.exe.");
	}
}

static BOOL port_open(int port)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = { 0 };
	BOOL open;

	address.sin_family = AF_INET;
	address.sin_port = htons((u_short)port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	open = connect(s, (struct sockaddr *)&address, sizeof(address)) == 0;
	closesocket(s);
	return open;
}

static BOOL port_free(int port)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = { 0 };
	BOOL exclusive = TRUE;
	BOOL free;

	address.sin_family = AF_INET;
	address.sin_port = htons((u_short)port);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
	free = bind(s, (struct sockaddr *)&address, sizeof(address)) == 0;
	closesocket(s);
	return free;
}

static int find_keys_port(void)
{
	int port;

	for (port = KEYS_PORT; port < KEYS_PORT + KEYS_PORT_TRIES; port++) {
		if (port_free(port)) {
			return port;
		}
	}
	fail(FALSE, "Ports %d to %d are all in use by other programs. Close them and try again.",
		KEYS_PORT, KEYS_PORT + KEYS_PORT_TRIES - 1);
	return -1;
}

static void start_keyboard(HANDLE qemu, int keys_port)
{
	char command[COMMAND_SIZE];
	ULONGLONG deadline = GetTickCount64() + KEYS_WAIT_MS;
	PROCESS_INFORMATION process;

	snprintf(command, sizeof(command), "keyboard.exe %d", keys_port);
	while (!port_open(keys_port)) {
		if (WaitForSingleObject(qemu, KEYS_POLL_MS) == WAIT_OBJECT_0) {
			return;
		}
		if (GetTickCount64() > deadline) {
			printf("The emulator did not open the keyboard port. Start \"keyboard.exe %d\" yourself.\n", keys_port);
			return;
		}
	}
	if (start_program(command, FALSE, &process)) {
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	} else {
		printf("Could not start keyboard.exe. Start \"keyboard.exe %d\" yourself.\n", keys_port);
	}
}

/* Ctrl+C also reaches QEMU, which shuts down. Keep running until it has exited. */
static BOOL WINAPI ignore_interrupt(DWORD type)
{
	return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
}

int main(int argc, char **argv)
{
	char folder[MAX_PATH];
	char hwi[MAX_PATH];
	PROCESS_INFORMATION qemu;
	WSADATA winsock;
	DWORD exit_code = 1;
	const char *problem;
	int keys_port;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("keysoft-emu %s\n", VERSION);
	if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
		return 0;
	}
	if (CreateMutexA(NULL, TRUE, INSTANCE_MUTEX) && GetLastError() == ERROR_ALREADY_EXISTS) {
		fail(FALSE, "start-emu is already running. Make sure that the previous process was fully closed by pressing control+c on the open console window.");
	}
	GetModuleFileNameA(NULL, folder, sizeof(folder));
	*strrchr(folder, '\\') = 0;
	SetCurrentDirectoryA(folder);
	if (check_firmware()) {
		find_hwi(hwi, sizeof(hwi));
		extract_firmware(hwi);
		problem = check_firmware();
		if (problem) {
			fail(TRUE, "%s", problem);
		}
		printf("The firmware is ready.\n");
	}
	create_nand_images();
	WSAStartup(MAKEWORD(2, 2), &winsock);
	SetConsoleCtrlHandler(ignore_interrupt, TRUE);
	keys_port = find_keys_port();
	start_emulator(&qemu, keys_port);
	start_keyboard(qemu.hProcess, keys_port);
	WaitForSingleObject(qemu.hProcess, INFINITE);
	GetExitCodeProcess(qemu.hProcess, &exit_code);
	return (int)exit_code;
}
