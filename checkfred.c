#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <linux/unistd.h>

bool check_fred(void)
{
	const uint64_t magic = 0xfeedfacedeadbeef;
	long nr_ret;
	uint64_t rcx;

	nr_ret = __NR_getppid;
	rcx    = magic;
	asm volatile("syscall" : "+a" (nr_ret), "+c" (rcx) : : "r11");

	return rcx == magic;
}

int main(void)
{
	bool is_fred = check_fred();

	printf("FRED is %s\n", is_fred ? "ON" : "OFF");
	return !is_fred;
}

