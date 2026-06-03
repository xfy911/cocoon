#include "unity.h"
#include "multipart.h"
#include <string.h>
#include <stdlib.h>

/* ===== multipart_extract_boundary ===== */

void test_extract_boundary_simple(void) {
    char buf[256];
    TEST_ASSERT_TRUE(multipart_extract_boundary(
        "multipart/form-data; boundary=----WebKitFormBoundary", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("----WebKitFormBoundary", buf);
}

void test_extract_boundary_quoted(void) {
    char buf[256];
    TEST_ASSERT_TRUE(multipart_extract_boundary(
        "multipart/form-data; boundary=\"abc123\"", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("abc123", buf);
}

void test_extract_boundary_with_extra_params(void) {
    char buf[256];
    TEST_ASSERT_TRUE(multipart_extract_boundary(
        "multipart/form-data; boundary=xyz; charset=utf-8", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("xyz", buf);
}

void test_extract_boundary_null(void) {
    char buf[256];
    TEST_ASSERT_FALSE(multipart_extract_boundary(NULL, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(multipart_extract_boundary("text/plain", buf, sizeof(buf)));
}

void test_extract_boundary_too_small_buffer(void) {
    char buf[4];
    TEST_ASSERT_FALSE(multipart_extract_boundary(
        "multipart/form-data; boundary=abcdef", buf, sizeof(buf)));
}

/* ===== multipart_parse ===== */

static const char *SAMPLE_MULTIPART =
    "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "Hello, World!\r\n"
    "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
    "Content-Disposition: form-data; name=\"description\"\r\n"
    "\r\n"
    "Test file upload\r\n"
    "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";

void test_parse_simple_multipart(void) {
    multipart_part_t *parts = NULL;
    int num_parts = 0;
    TEST_ASSERT_EQUAL(0, multipart_parse(SAMPLE_MULTIPART, strlen(SAMPLE_MULTIPART),
                                          "----WebKitFormBoundary7MA4YWxkTrZu0gW",
                                          &parts, &num_parts));
    TEST_ASSERT_EQUAL(2, num_parts);

    TEST_ASSERT_EQUAL_STRING("file", parts[0].name);
    TEST_ASSERT_EQUAL_STRING("test.txt", parts[0].filename);
    TEST_ASSERT_EQUAL_STRING("text/plain", parts[0].content_type);
    TEST_ASSERT_EQUAL(13, parts[0].data_len);
    TEST_ASSERT_EQUAL_STRING("Hello, World!", parts[0].data);

    TEST_ASSERT_EQUAL_STRING("description", parts[1].name);
    TEST_ASSERT_NULL(parts[1].filename);
    TEST_ASSERT_EQUAL(16, parts[1].data_len);
    TEST_ASSERT_EQUAL_STRING("Test file upload", parts[1].data);

    multipart_parts_free(parts, num_parts);
}

void test_parse_no_filename(void) {
    const char *body =
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1\r\n"
        "--boundary--\r\n";
    multipart_part_t *parts = NULL;
    int num_parts = 0;
    TEST_ASSERT_EQUAL(0, multipart_parse(body, strlen(body), "boundary", &parts, &num_parts));
    TEST_ASSERT_EQUAL(1, num_parts);
    TEST_ASSERT_EQUAL_STRING("field1", parts[0].name);
    TEST_ASSERT_NULL(parts[0].filename);
    TEST_ASSERT_EQUAL_STRING("value1", parts[0].data);
    multipart_parts_free(parts, num_parts);
}

void test_parse_empty_body(void) {
    multipart_part_t *parts = NULL;
    int num_parts = 0;
    TEST_ASSERT_EQUAL(-1, multipart_parse("", 0, "boundary", &parts, &num_parts));
}

void test_parse_null_args(void) {
    multipart_part_t *parts = NULL;
    int num_parts = 0;
    TEST_ASSERT_EQUAL(-1, multipart_parse(NULL, 0, "boundary", &parts, &num_parts));
    TEST_ASSERT_EQUAL(-1, multipart_parse("x", 1, NULL, &parts, &num_parts));
}

void test_parse_invalid_boundary(void) {
    const char *body = "not a multipart body";
    multipart_part_t *parts = NULL;
    int num_parts = 0;
    TEST_ASSERT_EQUAL(-1, multipart_parse(body, strlen(body), "boundary", &parts, &num_parts));
}

void test_parse_binary_data(void) {
    /* 包含 \0 的二进制数据 */
    char body[256];
    int n = snprintf(body, sizeof(body),
        "--bin\r\n"
        "Content-Disposition: form-data; name=\"bin\"; filename=\"data.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n");
    /* 追加二进制数据 */
    body[n++] = 0x00;
    body[n++] = 0x01;
    body[n++] = 0x02;
    body[n++] = 0x03;
    int end = snprintf(body + n, sizeof(body) - n, "\r\n--bin--\r\n");
    n += end;

    multipart_part_t *parts = NULL;
    int num_parts = 0;
    TEST_ASSERT_EQUAL(0, multipart_parse(body, (size_t)n, "bin", &parts, &num_parts));
    TEST_ASSERT_EQUAL(1, num_parts);
    TEST_ASSERT_EQUAL(4, parts[0].data_len);
    TEST_ASSERT_EQUAL(0x00, (unsigned char)parts[0].data[0]);
    TEST_ASSERT_EQUAL(0x01, (unsigned char)parts[0].data[1]);
    TEST_ASSERT_EQUAL(0x02, (unsigned char)parts[0].data[2]);
    TEST_ASSERT_EQUAL(0x03, (unsigned char)parts[0].data[3]);
    multipart_parts_free(parts, num_parts);
}

void test_parse_multiple_files(void) {
    const char *body =
        "--multi\r\n"
        "Content-Disposition: form-data; name=\"files\"; filename=\"a.txt\"\r\n"
        "\r\n"
        "A\r\n"
        "--multi\r\n"
        "Content-Disposition: form-data; name=\"files\"; filename=\"b.txt\"\r\n"
        "\r\n"
        "B\r\n"
        "--multi--\r\n";
    multipart_part_t *parts = NULL;
    int num_parts = 0;
    TEST_ASSERT_EQUAL(0, multipart_parse(body, strlen(body), "multi", &parts, &num_parts));
    TEST_ASSERT_EQUAL(2, num_parts);
    TEST_ASSERT_EQUAL_STRING("a.txt", parts[0].filename);
    TEST_ASSERT_EQUAL_STRING("b.txt", parts[1].filename);
    multipart_parts_free(parts, num_parts);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_extract_boundary_simple);
    RUN_TEST(test_extract_boundary_quoted);
    RUN_TEST(test_extract_boundary_with_extra_params);
    RUN_TEST(test_extract_boundary_null);
    RUN_TEST(test_extract_boundary_too_small_buffer);

    RUN_TEST(test_parse_simple_multipart);
    RUN_TEST(test_parse_no_filename);
    RUN_TEST(test_parse_empty_body);
    RUN_TEST(test_parse_null_args);
    RUN_TEST(test_parse_invalid_boundary);
    RUN_TEST(test_parse_binary_data);
    RUN_TEST(test_parse_multiple_files);

    return UNITY_END();
}
