#ifndef FBB_ADBLOCK
#define FBB_ADBLOCK

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Fbbadblock Fbbadblock;

// Creates empty handle which isnt ready, returns immediately and no rules are
// parsed
Fbbadblock *fbb_adblock_new(void);

// Turns lens bytes of ABP/uBO rules (\n separated) and swaps the result in as
// the active engine Network, cosmetic, and scriplet rules are all kept
// Resources and resources len is a JSON array of adblock-rust resources, may be
// NULL, where scriptlet rules produce no script. Rules added with trusted
// permissions CPU costly, keep calls to worker threads. Can be called to
// replace the ruleset Queries answered by prev engine until swap Returns false
// on error and prev engine used
bool fbb_adblock_load(Fbbadblock *, const char *rules, size_t len,
                      const char *resources, size_t resources_len);

// True once load succeeded, lock free
bool fbb_adblock_ready(const Fbbadblock *);

// Never waits for load, returns false whilst no engine ready
// Can be called from any thread, concurrently with fbb_adblock_load
bool fbb_adblock_should_block(const Fbbadblock *, const char *url,
                              const char *src, const char *type);

// Js to run in page main world at doc creation for url
// Returns NULL when nothing to inject
// Caller must free with fbb_adblock_string_free
// Can be called from any thread
char *fbb_adblock_page_script(const Fbbadblock *, const char *url);

void fbb_adblock_string_free(char *);

// Caller must make sure that no other thread is in use of the handle
void fbb_adblock_free(Fbbadblock *);

#ifdef __cplusplus
}
#endif

#endif // !FBB_ADBLOCK
