#include "tilefinch/resource_integrity.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "SRI CHECK failed at %s:%d: %s\n",               \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

int main(void)
{
    static const uint8_t body[] = "hello";
    static const char sha256[] =
        "sha256-LPJNul+wow4m6DsqxbninhsWHlwfp0JecwQzYpOLmCQ=";
    static const char sha384[] =
        "sha384-WeF0h3dEjGnea4ANejO7+5/xtGPkQ1TDVTvNucZm+pASWjx5+QOXvfX2oT3oKGhP";
    static const char sha512[] =
        "sha512-m3HSJL1i83hdltRq0+o9czGb+8KJDKra4t/3JRlnPKcjI8PZm6XBHXx6zG4UuMXaDEZjR1wuXDre9G9zvN7AQw==";
    CHECK(tilefinch_resource_integrity_verify(
              NULL, 0, body, sizeof(body) - 1)
          == TILEFINCH_INTEGRITY_NOT_ENFORCED);
    CHECK(tilefinch_resource_integrity_verify(
              "unknown-value", 13, body, sizeof(body) - 1)
          == TILEFINCH_INTEGRITY_NOT_ENFORCED);
    CHECK(tilefinch_resource_integrity_verify(
              sha256, sizeof(sha256) - 1, body, sizeof(body) - 1)
          == TILEFINCH_INTEGRITY_MATCH);
    CHECK(tilefinch_resource_integrity_verify(
              sha384, sizeof(sha384) - 1, body, sizeof(body) - 1)
          == TILEFINCH_INTEGRITY_MATCH);
    CHECK(tilefinch_resource_integrity_verify(
              sha512, sizeof(sha512) - 1, body, sizeof(body) - 1)
          == TILEFINCH_INTEGRITY_MATCH);

    static const char strongest_mismatch[] =
        "sha256-LPJNul+wow4m6DsqxbninhsWHlwfp0JecwQzYpOLmCQ= "
        "sha384-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    CHECK(tilefinch_resource_integrity_verify(
              strongest_mismatch, sizeof(strongest_mismatch) - 1,
              body, sizeof(body) - 1)
          == TILEFINCH_INTEGRITY_MISMATCH);
    static const char strongest_one_matches[] =
        "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA== "
        "sha512-m3HSJL1i83hdltRq0+o9czGb+8KJDKra4t/3JRlnPKcjI8PZm6XBHXx6zG4UuMXaDEZjR1wuXDre9G9zvN7AQw==";
    CHECK(tilefinch_resource_integrity_verify(
              strongest_one_matches, sizeof(strongest_one_matches) - 1,
              body, sizeof(body) - 1)
          == TILEFINCH_INTEGRITY_MATCH);
    CHECK(tilefinch_resource_integrity_verify(
              "sha256-not-base64!", 19, body, sizeof(body) - 1)
          == TILEFINCH_INTEGRITY_INVALID);
    CHECK(tilefinch_resource_integrity_verify(
              sha256, sizeof(sha256) - 1,
              (const uint8_t *) "HELLO", 5)
          == TILEFINCH_INTEGRITY_MISMATCH);
    puts("resource integrity tests passed");
    return 0;
}
