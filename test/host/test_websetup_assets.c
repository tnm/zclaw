/*
 * Host tests for web setup HTML assets shared by firmware and local preview.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

static char *read_file_text(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long size;
    char *buf;

    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fp);
    return buf;
}

TEST(setup_html_fields_and_route)
{
    const char *path = "../../main/web/setup.html";
    char *html = read_file_text(path);
    ASSERT(html != NULL);

    // Form structure
    ASSERT(strstr(html, "<form") != NULL);
    ASSERT(strstr(html, "action=\"/save\"") != NULL);

    // Key elements
    ASSERT(strstr(html, "id=\"backend-select\"") != NULL);
    ASSERT(strstr(html, "id=\"model-select\"") != NULL);
    ASSERT(strstr(html, "id=\"scan-btn\"") != NULL);
    ASSERT(strstr(html, "id=\"ssid-select\"") != NULL);
    ASSERT(strstr(html, "fetch(\"/networks\"") != NULL);

    // Form field names
    ASSERT(strstr(html, "name=\"ssid\"") != NULL);
    ASSERT(strstr(html, "name=\"pass\"") != NULL);
    ASSERT(strstr(html, "name=\"backend\"") != NULL);
    ASSERT(strstr(html, "name=\"apikey\"") != NULL);
    ASSERT(strstr(html, "name=\"model\"") != NULL);
    ASSERT(strstr(html, "name=\"tgtoken\"") != NULL);
    ASSERT(strstr(html, "name=\"tgchatid\"") != NULL);

    // Backend providers
    ASSERT(strstr(html, "Anthropic") != NULL);
    ASSERT(strstr(html, "OpenAI") != NULL);
    ASSERT(strstr(html, "OpenRouter") != NULL);

    // Some model options
    ASSERT(strstr(html, "claude-sonnet-4-5") != NULL);
    ASSERT(strstr(html, "gpt-5.2") != NULL);
    ASSERT(strstr(html, "deepseek/") != NULL);

    free(html);
    return 0;
}

TEST(success_html_copy)
{
    const char *path = "../../main/web/success.html";
    char *html = read_file_text(path);
    ASSERT(html != NULL);

    ASSERT(strstr(html, "Configuration Saved") != NULL);
    ASSERT(strstr(html, "connect to your WiFi network") != NULL);

    free(html);
    return 0;
}

int test_websetup_assets_all(void)
{
    int failures = 0;

    printf("\nWeb Setup Asset Tests:\n");

    printf("  setup_html_fields_and_route... ");
    if (test_setup_html_fields_and_route() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  success_html_copy... ");
    if (test_success_html_copy() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
