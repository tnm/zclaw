/*
 * Dedicated runner for real llm.c runtime tests.
 */

#include <stdio.h>

extern int test_llm_runtime_all(void);

int main(void)
{
    int failures;

    printf("zclaw LLM Runtime Tests\n");
    printf("=======================\n\n");

    failures = test_llm_runtime_all();

    printf("\n=======================\n");
    if (failures == 0) {
        printf("All tests passed!\n");
        return 0;
    }

    printf("%d test(s) failed\n", failures);
    return 1;
}
