/*
 * DNS for the setup network: every name has this device's address. A phone
 * that joins asks for a page it knows, is sent to the setup page instead, and
 * takes that to mean it should show the page.
 */
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "app.h"

static const char *TAG = "dns";

#define HEADER_LEN 12
#define TYPE_A 1
#define CLASS_IN 1
#define TTL_SECONDS 60

static uint32_t address; /* network byte order */

static unsigned get16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

/* Turns the query in buf into its answer; returns the answer's length, 0 for none. */
static int answer(uint8_t *buf, int len, int cap)
{
    /* a standard query with one question, which is all a resolver sends */
    if (len < HEADER_LEN || (buf[2] & 0xF8) || get16(buf + 4) != 1) return 0;

    int end = HEADER_LEN;
    while (end < len && buf[end]) {
        if (buf[end] & 0xC0) return 0; /* a question has no reason to be compressed */
        end += buf[end] + 1;
    }
    end += 5; /* the root label, type and class */
    if (end > len) return 0;

    bool known = get16(buf + end - 4) == TYPE_A && get16(buf + end - 2) == CLASS_IN;
    buf[2] = 0x84 | (buf[2] & 1); /* response, authoritative, recursion desired as asked */
    buf[3] = 0x80;                /* recursion available, no error */
    memset(buf + 6, 0, 6);
    buf[7] = known; /* one answer, or none: there is no IPv6 here to give an address for */
    if (!known) return end;

    static const uint8_t record[] = {0xC0, HEADER_LEN, 0, TYPE_A, 0, CLASS_IN, 0, 0, 0, TTL_SECONDS, 0, 4};
    if (end + (int)sizeof(record) + 4 > cap) return 0;
    memcpy(buf + end, record, sizeof(record));
    memcpy(buf + end + sizeof(record), &address, 4);
    return end + sizeof(record) + 4;
}

static void dns_task(void *arg)
{
    static uint8_t buf[512];
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "no socket: phones will not be shown the setup page by themselves");
        vTaskDelete(NULL);
    }
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n > 0 && (n = answer(buf, n, sizeof(buf))) > 0) sendto(s, buf, n, 0, (struct sockaddr *)&from, from_len);
    }
}

void dns_start(uint32_t ip)
{
    address = ip;
    xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 4, NULL, 0);
}
