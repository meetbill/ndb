/*
 * file   : test_array.c
 * author : ning
 * date   : 2014-06-30 17:26:37
 */

#include "nc_util.h"
#include "testhelp.h"

/*********************** test array *************************/
static rstatus_t
array_check(void *elem, void *data)
{
    int *pi = elem;

    printf("got pi: %p %d\n", elem, *pi);
    return 0;
}

static void
test_array()
{
    array_t *arr;
    int i;
    int *pi;

    arr = array_create(3, sizeof(int));
    for (i = 0; i < 5; i++) {
        pi = array_push(arr);
        *pi = i;
    }

    for (i = 0; i < 5; i++) {
        pi = array_get(arr, i);
        TEST_ASSERT("assert_n", i == *pi);
    }
    TEST_ASSERT("array_n", 5 == array_n(arr));

    array_each(arr, array_check, NULL);
    arr->nelem = 0;
    array_destroy(arr);
}

/************************* test log *************************/
static const char *log_file = "/tmp/test_log.log";
static void
log_clean()
{
    char buf[1024];

    nc_scnprintf(buf, sizeof(buf), "rm %s", log_file);
    system(buf);
}

static int
str_count(const char *haystack, const char *needle)
{
    char *p = (char *)haystack;
    int cnt = 0;
    int needle_len = strlen(needle);

    for (; *p; p++) {
        if (0 == strncmp(needle, p, needle_len))
            cnt++;
    }
    return cnt;
}

static void
test_log()
{
    FILE *f;
    char buf[1024];

    log_init(LOG_NOTICE, (char *)log_file);
    log_clean();

    log_reopen(); /* create new file */

    loga("1. loga");
    log_debug("2. debug log");
    log_notice("3. notice log");
    log_warn("4. warn log");

    f = fopen(log_file, "r");
    if (f == NULL) {
        TEST_ASSERT("open file", 0);
    }
    fread(buf, 1, sizeof(buf), f);

    TEST_ASSERT("cnt-line",
              3 == str_count(buf, "\n"));
    TEST_ASSERT("loga",
              strstr(buf, "loga"));

    TEST_ASSERT("debug",
              !strstr(buf, "debug"));

    TEST_ASSERT("notice",
              strstr(buf, "notice"));

    TEST_ASSERT("warn",
              strstr(buf, "warn"));
}

typedef struct mytimer_s {
    struct rbnode timer_rbe;    /* timeout rbtree sentinel */
    /* int           time; */
} mytimer_t;

static void
test_rbtree()
{
    struct rbtree timer_rbt;                    /* timeout rbtree */
    struct rbnode timer_rbs;                    /* timeout rbtree sentinel */
    mytimer_t *timer;
    struct rbnode *nodep;                       /* timeout rbtree sentinel */
    uint64_t timers[] = { 1, 5, 3, 2, 8 };
    uint64_t timers_sorted[] = { 1, 2, 3, 5, 8 };
    int i;

    rbtree_init(&timer_rbt, &timer_rbs);

    for (i = 0; i < sizeof(timers) / sizeof(uint64_t); i++) {
        timer = nc_alloc(sizeof(mytimer_t));
        rbtree_node_init(&timer->timer_rbe);
        timer->timer_rbe.key = timers[i];
        rbtree_insert(&timer_rbt, &timer->timer_rbe);
    }

    for (i = 0; i < sizeof(timers) / sizeof(uint64_t); i++) {
        nodep = rbtree_min(&timer_rbt);
        printf("key: %" PRIu64 " \n ", nodep->key);
        TEST_ASSERT("test_rbtree key",
                  timers_sorted[i] == nodep->key);
        rbtree_delete(&timer_rbt, nodep);
    }
}

static void
test_md5()
{
    /* TODO */
}

int
main()
{
    test_log();
    test_array();
    test_rbtree();
    test_md5();

    test_report();
    return 0;
}

/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */
