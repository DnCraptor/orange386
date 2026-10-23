#include "input_linux.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES   16
#define KEY_QUEUE     256
#define RESCAN_US     1000000u

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NLONGS(x)     (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, array) \
	((array[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

struct input_dev {
	int fd;
	char path[32];
	bool keyboard;
	bool mouse;
};

static struct input_dev devs[MAX_DEVICES];
static int ndevs;
static uint64_t last_scan_us;
static bool initialized;

static struct { uint16_t code; uint8_t down; } keyq[KEY_QUEUE];
static unsigned keyq_r, keyq_w;

static int32_t mouse_dx, mouse_dy, mouse_dz;
static uint8_t mouse_buttons;
static bool mouse_changed;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static bool is_open(const char *path)
{
	for (int i = 0; i < ndevs; ++i)
		if (!strcmp(devs[i].path, path))
			return true;
	return false;
}

static void probe(const char *path)
{
	if (ndevs >= MAX_DEVICES || is_open(path))
		return;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;

	unsigned long ev[NLONGS(EV_CNT)] = {0};
	unsigned long keys[NLONGS(KEY_CNT)] = {0};
	unsigned long rel[NLONGS(REL_CNT)] = {0};
	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0) {
		close(fd);
		return;
	}
	if (TEST_BIT(EV_KEY, ev))
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
	if (TEST_BIT(EV_REL, ev))
		ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);

	bool keyboard = TEST_BIT(EV_KEY, ev) && TEST_BIT(KEY_A, keys) &&
			TEST_BIT(KEY_Z, keys) && TEST_BIT(KEY_SPACE, keys);
	bool mouse = TEST_BIT(EV_REL, ev) && TEST_BIT(REL_X, rel) &&
		     TEST_BIT(REL_Y, rel) && TEST_BIT(BTN_LEFT, keys);
	if (!keyboard && !mouse) {
		close(fd);
		return;
	}
	/* Exclusive access: the console must not see the guest's input. */
	if (ioctl(fd, EVIOCGRAB, 1) < 0)
		fprintf(stderr, "input: cannot grab %s: %s\n", path, strerror(errno));

	char name[64] = "?";
	ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	printf("input: %s \"%s\"%s%s\n", path, name,
	       keyboard ? " keyboard" : "", mouse ? " mouse" : "");

	struct input_dev *d = &devs[ndevs++];
	d->fd = fd;
	snprintf(d->path, sizeof(d->path), "%s", path);
	d->keyboard = keyboard;
	d->mouse = mouse;
}

static void rescan(void)
{
	DIR *dir = opendir("/dev/input");
	if (!dir)
		return;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		char path[32];
		snprintf(path, sizeof(path), "/dev/input/%.20s", de->d_name);
		probe(path);
	}
	closedir(dir);
}

static void drop(int i)
{
	printf("input: %s removed\n", devs[i].path);
	close(devs[i].fd);
	devs[i] = devs[--ndevs];
}

static void push_key(uint16_t code, int value)
{
	unsigned next = (keyq_w + 1) % KEY_QUEUE;
	if (next == keyq_r)
		return;   /* queue full: drop, like a full PS/2 buffer */
	keyq[keyq_w].code = code;
	keyq[keyq_w].down = value != 0;
	keyq_w = next;
}

static void handle(const struct input_event *e)
{
	switch (e->type) {
	case EV_KEY:
		if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE) {
			uint8_t bit = e->code == BTN_LEFT ? 0x01 :
				      e->code == BTN_RIGHT ? 0x02 : 0x04;
			uint8_t b = e->value ? (mouse_buttons | bit) : (mouse_buttons & ~bit);
			if (b != mouse_buttons) {
				mouse_buttons = b;
				mouse_changed = true;
			}
		} else if (e->code < BTN_MISC) {
			push_key(e->code, e->value);
		}
		break;
	case EV_REL:
		if (e->code == REL_X)
			mouse_dx += e->value;
		else if (e->code == REL_Y)
			mouse_dy += e->value;
		else if (e->code == REL_WHEEL)
			mouse_dz -= e->value;   /* evdev: + = away from the user */
		else
			break;
		mouse_changed = true;
		break;
	default:
		break;
	}
}

void input_init(void)
{
	if (initialized)
		return;
	initialized = true;
	rescan();
	last_scan_us = now_us();
}

void input_poll(void)
{
	if (!initialized)
		return;
	uint64_t now = now_us();
	if (now - last_scan_us >= RESCAN_US) {
		last_scan_us = now;
		rescan();
	}
	for (int i = 0; i < ndevs; ) {
		struct input_event ev[64];
		ssize_t n = read(devs[i].fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				++i;
				continue;
			}
			drop(i);   /* ENODEV: unplugged */
			continue;
		}
		for (ssize_t k = 0; k < n / (ssize_t)sizeof(ev[0]); ++k)
			handle(&ev[k]);
		if (n < (ssize_t)sizeof(ev))
			++i;
	}
}

bool input_get_key(int *is_down, int *keycode)
{
	if (keyq_r == keyq_w)
		return false;
	*keycode = keyq[keyq_r].code;
	*is_down = keyq[keyq_r].down;
	keyq_r = (keyq_r + 1) % KEY_QUEUE;
	return true;
}

static int16_t clamp16(int32_t v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

bool input_get_mouse(int16_t *dx, int16_t *dy, int8_t *dz, uint8_t *buttons)
{
	if (!mouse_changed)
		return false;
	*dx = clamp16(mouse_dx);
	*dy = clamp16(mouse_dy);
	*dz = (int8_t)(mouse_dz > 127 ? 127 : mouse_dz < -127 ? -127 : mouse_dz);
	*buttons = mouse_buttons;
	mouse_dx = mouse_dy = mouse_dz = 0;
	mouse_changed = false;
	return true;
}

void input_shutdown(void)
{
	for (int i = 0; i < ndevs; ++i) {
		ioctl(devs[i].fd, EVIOCGRAB, 0);
		close(devs[i].fd);
	}
	ndevs = 0;
	initialized = false;
}
