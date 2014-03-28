#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "git2.h"

#include "common.h"

#ifdef IMPL_THREAD
#include "naive.h"
#elif IMPL_CUDA
#include "cuda.h"
#else
#include "gpgpu.h"
#endif

git_repository *repo = NULL;

char volatile stop = 0;
char updated = 0;
git_oid *push_commit;

pthread_mutex_t commit_mutex;
pthread_mutex_t update_mutex;

// Copied from libgit2 examples
static int log_lg2(int error, const char *message, const char *extra){
    const git_error *lg2err;
    const char *lg2msg = "", *lg2spacer = "";

    if (!error)
        return error;

    if ((lg2err = giterr_last()) != NULL && lg2err->message != NULL) {
        lg2msg = lg2err->message;
        lg2spacer = " - ";
    }

    if (extra)
        fprintf(stderr, "%s '%s' [%d]%s%s\n",
                message, extra, error, lg2spacer, lg2msg);
    else
        fprintf(stderr, "%s [%d]%s%s\n",
                message, error, lg2spacer, lg2msg);

    return error;
}

static void check_lg2(int error, const char *message, const char *extra)
{
    log_lg2(error, message, extra);
    if(error){
        exit(1);
    }
}

static void reset_hard(){
    git_object *remote_commit;
    git_reference *remote_head;

    check_lg2(git_reference_lookup(&remote_head, repo, "refs/remotes/origin/master"),
              "Could not lookup master branch", NULL);

    check_lg2(git_reference_peel((git_object**)&remote_commit, remote_head, GIT_OBJ_COMMIT),
              "Could not peel remote commit", NULL);
    check_lg2(git_reset(repo, (git_object *)remote_commit, GIT_RESET_HARD),
              "Could not reset to remote head", NULL);

    git_object_free(remote_commit);
    git_reference_free(remote_head);
}

static int prepare_index(git_index *index, char* msg){
    git_oid index_tree;
    git_tree *head_obj;
    git_reference *head;
    char tree_str[GIT_OID_HEXSZ+1], parent_str[GIT_OID_HEXSZ+1];

    // Get the head OID
    git_repository_head(&head, repo);
    git_reference_peel((git_object**)&head_obj, head, GIT_OBJ_COMMIT);
    git_oid_tostr(parent_str, GIT_OID_HEXSZ+1,
                   git_tree_id(head_obj));

    git_reference_free(head);
    git_object_free((git_object*)head_obj);

    // Write the coin
    // TODO: Use C for this?
    system("perl -i -pe 's/(breaking_barrie: )(\\d+)/$1 . ($2+1)/e' LEDGER.txt");
    system("grep -q \"breaking_barrie\" LEDGER.txt || echo \"breaking_barrie: 1\" >> LEDGER.txt");

    // Update the index
    check_lg2(git_index_read(index, 0),
              "Could not re-read index from disk", NULL);

    check_lg2(git_index_add_bypath(index, "LEDGER.txt"),
              "Could not add to index", "LEDGER.txt");

    // Write the index and get the tree OID
    git_index_write_tree(&index_tree, index);
    git_oid_tostr(tree_str, GIT_OID_HEXSZ+1, &index_tree);

    snprintf(msg, BUFFER_LENGTH,
             "commit %d%c"
             "tree %s\n"
             "parent %s\n"
             "author Barrie Breaker <mbarrie@gmail.com> %d +0000\n"
             "committer Barrie Breaker <mbarrie@gmail.com> %d +0000\n"
             "\n"
             "Brute it!"
             "\x01\x01\x01\x01"
             "\x01\x01\x01\x01"
             "\x01\x01\x01", // Align 32 bit words on GPU
             MSG_LENGTH, 0, tree_str, parent_str,
             (int)time(NULL), (int)time(NULL));
    pad_message(msg, COMMIT_LENGTH, BUFFER_LENGTH);

    return 0;
}

static void fetch_updates(){
    FILE *fp;

    fp = popen("git fetch -q 2>/dev/null", "r");

    if(pclose(fp) == -1){
        puts("Error closing fetch pipe");
        exit(1);
    }
}

static int check_updates(){
    git_reference *before_head, *after_head;
    int retval;

    check_lg2(git_reference_lookup(&before_head, repo, "refs/remotes/origin/master"),
              "Could not lookup master branch (before)", NULL);

    fetch_updates();

    check_lg2(git_reference_lookup(&after_head, repo, "refs/remotes/origin/master"),
              "Could not lookup master branch (after)", NULL);

    // Fetch updated things
    if(git_reference_cmp(before_head, after_head)){
        puts("Update detected");
        retval = 1;
    } else {
        retval = 0;
    }

    git_reference_free(before_head);
    git_reference_free(after_head);

    return retval;
}

static int record_push_status_cb(const char *ref, const char *msg, void *retval){
    if(msg){
        printf("Push: '%s' failed with %s\n", ref, msg);
        (*(int*)retval) |= 1;
    }
    return 0;
}

static int cred_acquire_cb(git_cred **cred, const char *url,
                           const char *user_from_url,
                           unsigned int allowed_types,
                           void *payload){
    return git_cred_ssh_none_new(cred, user_from_url);
}

static int push_result_lib(git_oid *commit, git_remote *remote){
    git_push *push;
    int retval = 0;
    char cmd[128];

    git_oid_tostr(cmd, GIT_OID_HEXSZ+1, commit);
    strncat(cmd, ":refs/heads/master", 128);
    puts(cmd);

    check_lg2(git_push_new(&push, remote),
              "Error creating push", NULL);
    check_lg2(git_push_add_refspec(push, cmd),
              "Failed to add refspec to push", NULL);

    retval = log_lg2(git_push_finish(push),
                     "Failed to finish push", NULL);

    if(retval || !git_push_unpack_ok(push)){
        puts("Push: unpack failed!");
        retval = 1;
    } else {
        git_push_status_foreach(push, record_push_status_cb, &retval);
    }

    git_push_free(push);

    return retval;
}

static int push_result_shell(git_oid *commit, git_remote *remote){
    char cmd[128], oid[GIT_OID_HEXSZ+1];
    FILE *fp;

    git_oid_tostr(oid, GIT_OID_HEXSZ+1, commit);

    snprintf(cmd, 128, "git push origin %s:master", oid);

    fp = popen(cmd, "r");

    return pclose(fp);
}

static void* check_updates_worker(void* arg){
    timing_info timing;
    reset_timing(&timing);

    while(!stop){
        start_timing(&timing);

        if(!updated && push_commit){
            if(push_commit){
                pthread_mutex_lock(&commit_mutex);

                if(push_result_shell(push_commit, NULL)){
                    puts("Failed to push coin, reseting.");

                    pthread_mutex_lock(&update_mutex);

                    fetch_updates();
                    updated = 1;

                    pthread_mutex_unlock(&update_mutex);
                } else {
                    puts("Earned it!");
                }
                push_commit = NULL;

                pthread_mutex_unlock(&commit_mutex);
            } else {
                if(check_updates()){
                    pthread_mutex_lock(&update_mutex);
                    updated = 1;
                    pthread_mutex_unlock(&update_mutex);
                }
            }
            time_point(&timing);
            print_timing(&timing);
        } else {
            skip_point(&timing);
            usleep(10);
        }
    }

    puts("Update thread ending");

    pthread_exit(NULL);
}

static void int_handler(int sig){
    // Not super threadsafe but meh
    stop = 1;
    updated = 1;
}

static void commit_result(char* msg, git_oid *commit){
    git_odb *odb;

    check_lg2(git_repository_odb(&odb, repo),
              "Could not allocate odb poiner", NULL);

    check_lg2(git_odb_write(commit, odb, &msg[PREAMBLE_LENGTH], MSG_LENGTH, GIT_OBJ_COMMIT),
              "Error writing commit", NULL);

    check_lg2(git_reference_create(NULL, repo, "refs/heads/master", commit, 1),
              "Could not update head", NULL);

    git_odb_free(odb);
}

static unsigned char init_args(hash_args *args){
    FILE *fp;
    unsigned char difficulty;
    char hex_difficulty[SHA_DIGEST_LENGTH*2];

    fp = fopen("difficulty.txt", "r");
    fscanf(fp, "%40c", &hex_difficulty);
    difficulty = parse_difficulty(hex_difficulty);

    args->msg = malloc(BUFFER_LENGTH);
    args->stop = &updated;
    args->device_id = 0;

    return difficulty;
}

static void init_git(git_index **index){
    git_threads_init();

    check_lg2(git_repository_open(&repo, "."),
              "No git repository", NULL);
    check_lg2(git_repository_index(index, repo),
              "Could not open repository index", NULL);
}

int main (int argc, char **argv) {
    git_index *index = NULL;
    pthread_t updateThread;
    pthread_t hashThread[NUM_DEVICES];
    int rc, difficulty;
    void *status;
    hash_args args[NUM_DEVICES];
    timing_info timing;
    git_oid curr_commit;
    uint32_t i;
    int found_gpu;

    reset_timing(&timing);

    pthread_mutex_init(&commit_mutex, NULL);
    pthread_mutex_init(&update_mutex, NULL);
    push_commit = NULL;

    difficulty = init_args(&args[0]);
    for (i = 1; i < NUM_DEVICES; ++i) {
        args[i].msg = malloc(BUFFER_LENGTH);
        args[i].stop = args[0].stop;
        args[i].found = 0;
        args[i].device_id = i;
    }
    init_git(&index);

    init_hasher(difficulty);

    check_updates();
    reset_hard();

    puts("Starting update thread");
    rc = pthread_create(&updateThread, NULL, check_updates_worker, NULL);
    if (rc){
        printf("ERROR creating update thread %d\n", rc);
        exit(-1);
    }

    //signal (SIGINT, int_handler);

    while(!stop){
        start_timing(&timing);

        for (i = 0; i < NUM_DEVICES; ++i) {
            args[i].found = 0;
        }
        found_gpu = -1;

        time_point(&timing);

        pthread_mutex_lock(&commit_mutex);
        pthread_mutex_lock(&update_mutex);
        if(updated){
            reset_hard();
            updated = 0;
            push_commit = NULL;
        }
        pthread_mutex_unlock(&update_mutex);
        pthread_mutex_unlock(&commit_mutex);

        time_point(&timing);

        puts("Preparing index");
        prepare_index(index, args[0].msg);
        for (i = 1; i < NUM_DEVICES; ++i) {
            strcpy(args[i].msg, args[0].msg);
        }
        time_point(&timing);

        printf("Starting %d brute force thread(s)\n", NUM_DEVICES);
        for (i = 0; i < NUM_DEVICES; ++i) {
            rc = pthread_create(&hashThread[i], NULL, force_hash, &args[i]);
            if (rc){
                printf("ERROR creating hash thread %d\n", rc);
                stop = 1;
                break;
            }
        }

        time_point(&timing);

        while (found_gpu < 0) {
            for (i = 0; i < NUM_DEVICES; ++i) {
                if (args[i].found) {
                    found_gpu = i;
                    break;
                }
            }
        }

        time_point(&timing);

        if(!stop && !updated && found_gpu != -1){
            printf("Found one on device %d!\n", found_gpu);

            while(push_commit){
                usleep(10);
            }

            time_point(&timing);

            if(!stop && !updated){
                pthread_mutex_lock(&commit_mutex);

                commit_result(args[found_gpu].msg, &curr_commit);
                push_commit = &curr_commit;

                pthread_mutex_unlock(&commit_mutex);
            }
        } else {
            puts("Reset while looking for a hash");
            time_point(&timing);
        }

        time_point(&timing);
        print_timing(&timing);
    }

    pthread_join(updateThread, &status);

    free_hasher();
    for (i = 0; i < NUM_DEVICES; ++i) {
        free(args[i].msg);
    }

    git_index_free(index);
    git_repository_free(repo);

    git_threads_shutdown();

    return 0;
}
