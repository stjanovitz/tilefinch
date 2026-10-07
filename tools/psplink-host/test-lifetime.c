/* Mock the exact upstream call sites. Never initializes or contacts USB. */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libusb.h>

static int fake_bulk(libusb_device_handle *, unsigned char, unsigned char *,
                     int, int *, unsigned int);
static int fake_release(libusb_device_handle *, int);
static int fake_reset(libusb_device_handle *);
static void fake_close(libusb_device_handle *);
#define libusb_bulk_transfer fake_bulk
#define libusb_release_interface fake_release
#define libusb_reset_device fake_reset
#define libusb_close fake_close
#define main upstream_main
#include "usbhostfs_pc/main.c"
#undef main

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int token, second_token;
static libusb_device_handle *expected;
static bool entered, permit, retirement_started, closed;
static unsigned transfers, releases, resets, closes, in_flight;

static int fake_bulk(libusb_device_handle *dev, unsigned char ep,
                     unsigned char *data, int size, int *written, unsigned int timeout)
{
    (void)data;
    CHECK(dev == expected && ep == 3 && size == 4 && timeout == 10000);
    pthread_mutex_lock(&gate);
    CHECK(!closed); in_flight++; transfers++; entered = true;
    pthread_cond_broadcast(&changed);
    while (!permit) pthread_cond_wait(&changed, &gate);
    CHECK(!closed); in_flight--; *written = size;
    pthread_mutex_unlock(&gate);
    return 0;
}
static int fake_release(libusb_device_handle *dev, int iface)
{
    pthread_mutex_lock(&gate);
    CHECK(dev == expected && iface == 0 && !in_flight && !closed);
    releases++;
    pthread_mutex_unlock(&gate);
    return 0;
}
static int fake_reset(libusb_device_handle *dev)
{
    CHECK(dev == expected && releases == resets + 1); resets++; return 0;
}
static void fake_close(libusb_device_handle *dev)
{
    pthread_mutex_lock(&gate);
    CHECK(dev == expected && !in_flight && !closed && resets == closes + 1);
    closes++; closed = true;
    pthread_mutex_unlock(&gate);
}
static int transmit(void)
{
    char bytes[4] = {1, 2, 3, 4};
#ifdef UNSAFE_BASELINE
    /* Exact pre-fix async path: unlocked shared-pointer read and submission. */
    return usbhdr ? euid_usb_bulk_write(usbhdr, 3, bytes, 4, 10000)
                  : LIBUSB_ERROR_NO_DEVICE;
#else
    return write_async_usb(bytes, 4);
#endif
}
static void *writer(void *opaque)
{
    (void)opaque;
    CHECK(transmit() == 4);
    return NULL;
}
static void *retirement(void *opaque)
{
    (void)opaque;
    pthread_mutex_lock(&gate);
    retirement_started = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
#ifdef UNSAFE_BASELINE
    close_device(usbhdr); usbhdr = NULL;
#else
    retire_device();
#endif
    return NULL;
}
int main(void)
{
    alarm(10);
    CHECK(transmit() == LIBUSB_ERROR_NO_DEVICE && transfers == 0);
    for (unsigned iteration = 0; iteration < 200; iteration++) {
        expected = (libusb_device_handle *)(iteration & 1 ? &token : &second_token);
        closed = entered = permit = retirement_started = false;
        publish_device(expected);
        pthread_t writing, retiring;
        CHECK(!pthread_create(&writing, NULL, writer, NULL));
        pthread_mutex_lock(&gate);
        while (!entered) pthread_cond_wait(&changed, &gate);
        pthread_mutex_unlock(&gate);
        /* This forced submission window is exactly where the crash occurred.
         * The unmodified async writer fails here deterministically. */
        CHECK(pthread_mutex_trylock(&usb_lifetime_mutex) == EBUSY);
        CHECK(!pthread_create(&retiring, NULL, retirement, NULL));
        pthread_mutex_lock(&gate);
        while (!retirement_started) pthread_cond_wait(&changed, &gate);
        CHECK(closes == iteration && !closed && in_flight == 1);
        permit = true; pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&gate);
        CHECK(!pthread_join(writing, NULL));
        CHECK(!pthread_join(retiring, NULL));
        CHECK(usbhdr == NULL && closed && !in_flight && closes == iteration + 1);
        CHECK(transmit() == LIBUSB_ERROR_NO_DEVICE && transfers == iteration + 1);
    }
    /* A signal received by a thread holding the lifetime mutex must exit
     * without trying to take that mutex or calling libusb/stdio cleanup. */
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        signal(SIGTERM, signal_handler);
        pthread_mutex_lock(&usb_lifetime_mutex);
        raise(SIGTERM);
        _exit(99);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    printf("host-only lifetime: %u transfers, %u retirements; reconnect, refusal and signal passed\n",
           transfers, closes);
    return 0;
}
