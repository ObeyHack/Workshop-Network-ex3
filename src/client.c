#include "client.h"

////////////////////////// Helper Functions ////////////////////////////////////

bool send_data_str(void *kv_handle, char* buf){
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;
    ctx.buf = buf;
    ctx.size = (int) strlen(buf) + 1;

    unsigned int ctx_flag = IBV_SEND_SIGNALED;
    if (ctx.size < MAX_INLINE) {
        ctx_flag = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    }
    if (flagged_pp_post_send(&ctx, ctx_flag)) {
        fprintf(stderr, "Client couldn't post send\n");
        return EXIT_FAILURE;
    }

    // wait for completion
    if (pp_wait_completions(&ctx, 1)) {
        fprintf(stderr, "Client couldn't wait for completions\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

bool send_ACK(kvHandle* kv_handle){
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;
    unsigned int ctx_flag = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    int size = ctx.size;
    ctx.size = 1;
    if (flagged_pp_post_send(&ctx, ctx_flag)) {
        fprintf(stderr, "Server couldn't post send\n");
        return EXIT_FAILURE;
    }
    pp_wait_completions(&ctx, 1);
    ctx.size = size;
    return EXIT_SUCCESS;
}

////////////////////////// Client Functions ////////////////////////////////////

bool client_set_eager(void *kv_handle, const char *key, const char *value){
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;
    // send eager message
    char* flag = "se";
    sprintf(ctx.buf, "%s:%s:%s%c", flag, key, value, '\0');

    // send on the wire
    if (send_data_str(kv_handle, ctx.buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    // wait for ACK
    if (pp_wait_completions(&ctx, 1) == EXIT_FAILURE){
        fprintf(stderr, "Client couldn't wait for completions\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

bool client_set_rendezvous(void *kv_handle, const char *key, const char *value){
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;

    // 1. send malloc size, Format: "sr:key:valueSize\0"
    char* msg_size = (char*)malloc(10);
    sprintf(msg_size, "%lu", strlen(value));
    char* flag = "sr";
    sprintf(ctx.buf, "%s:%s:%s%c", flag, key, msg_size, '\0');
    if (send_data_str(kv_handle, ctx.buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    // 2. register memory
    struct ibv_mr* mr = ibv_reg_mr(ctx.pd, (void*) value, strlen(value), IBV_ACCESS_REMOTE_READ);
    if (!mr){
        fprintf(stderr, "Couldn't register memory region\n");
        return EXIT_FAILURE;
    }


    // 3. Receive msg from server. Format: "rkey:addr\0"
    uint64_t r_addr;
    uint32_t rkey;
    pp_wait_completions(&ctx, 1);
    sscanf(ctx.buf, "%u:%lu", &rkey, &r_addr);

    // 4. Write the value to the server
    if (pp_post_rdma_write(&ctx, mr, r_addr, rkey) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }
    pp_wait_completions(&ctx, 1);

    // 5. Deregister memory
    if (ibv_dereg_mr(mr)){
        fprintf(stderr, "Couldn't deregister memory region\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

bool client_get_rendezvous(void *kv_handle, const char *key, char **value, int size){
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;

    // 1. malloc a buffer of size value_size
    char* value_buf = (char*)malloc(size + 1);
    if (value_buf == NULL){
        return EXIT_FAILURE;
    }
    value_buf[size] = '\0';

    // 2. register the buffer
    struct ibv_mr* mr = ibv_reg_mr(ctx.pd, value_buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr){
        fprintf(stderr, "Couldn't register memory region\n");
        return EXIT_FAILURE;
    }

    // 3. Receive msg from server. Format: "rkey:addr\0"
    uint64_t r_addr;
    uint32_t rkey;
    pp_wait_completions(&ctx, 1);
    sscanf(ctx.buf, "%u:%lu", &rkey, &r_addr);

    // 4. Read the value from the server
    if (pp_post_rdma_read(&ctx, mr, r_addr, rkey) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }
    pp_wait_completions(&ctx, 1);

    // 6. De-register memory
    if (ibv_dereg_mr(mr)){
        fprintf(stderr, "Couldn't deregister memory region\n");
        return EXIT_FAILURE;
    }

    // 7. Copy the value to the value pointer
    *value = value_buf;
    return EXIT_SUCCESS;
}

////////////////////////// Server Functions ////////////////////////////////////

/////////// Get Server  /////////////

bool server_get_rendezvous(Database* db, kvHandle* kv_handle, Value* value){
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;

    // 1. send malloc size, Format: "r:valueSize\0"
    sprintf(ctx.buf, "r:%d", value->size);
    if (send_data_str(kv_handle, ctx.buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    // 2. send rkey and raddr. Format: rkey:raddr\0
    sprintf(ctx.buf, "%u:%lu", value->mr->rkey, (u_int64_t)value->mr->addr);
    if (send_data_str(kv_handle, ctx.buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

bool server_get_eager(kvHandle* kv_handle, Value* value){
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;
    sprintf(ctx.buf, "e:%s", value->value);
    send_data_str(kv_handle, ctx.buf);
    return EXIT_SUCCESS;
}

bool server_get_FIN(Database* db, kvHandle* kv_handle, char* key){
    //decrease the num_in_get
    remove_num_in_get(db, key);
}
/////////// Set Server  /////////////

bool server_set_rendezvous(Database* db, kvHandle* kv_handle, char* key, int value_size){
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;

    // 1. malloc a buffer of size value_size
    char* value = (char*)malloc(value_size + 1);
    value[value_size] = '\0';

    // 2. register the buffer
    struct ibv_mr* mr = ibv_reg_mr(ctx.pd, (void*) value, value_size, IBV_ACCESS_LOCAL_WRITE |
                                                                         IBV_ACCESS_REMOTE_READ |
                                                                         IBV_ACCESS_REMOTE_WRITE);
    if (!mr){
        fprintf(stderr, "Couldn't register memory region\n");
        return EXIT_FAILURE;
    }

    // 3. send memory region, Format: "rkey:addr\0"
    sprintf(ctx.buf, "%u:%lu", mr->rkey, (u_int64_t) mr->addr);
    if (send_data_str(kv_handle, ctx.buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    // 4. Set the item in the database
    Value *value_struct = (Value *) malloc(sizeof(Value));
    if (value_struct == NULL) {
        return EXIT_FAILURE;
    }
    value_struct->value = value;
    value_struct->size = value_size;
    value_struct->is_large = true;
    value_struct->mr = mr;
    if (set_item(db, key, value_struct) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

bool server_set_eager(Database* db, kvHandle* kv_handle, char* key, char* value){
    Value *value_struct = (Value *) malloc(sizeof(Value));
    if (value_struct == NULL) {
        return EXIT_FAILURE;
    }
    value_struct->value = value;
    value_struct->size = strlen(value);
    value_struct->is_large = false;
    value_struct->mr = NULL;
    if (set_item(db, key, value_struct) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }
    // send ACK
    if (send_ACK(kv_handle) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

bool server_set_FIN(Database* db, kvHandle* kv_handle, char* key){
    // decrease the num_in_set
    remove_num_in_set(db, key);
}
/////////// Receive Query  /////////////
//send again buffer
char *send_again_buffer[256];
kvHandle* send_again_kv_handle[256];
int send_again_index = 0;

bool parse_data(Database* db, kvHandle* kv_handle, char* buf){
//    char buffer[4*KB];
//    strcpy(buffer, buf);

    char* buffer = buf;
    // Data format: flag:key:x
    char* flag = strtok(buffer, ":");

    if (strlen(flag) != 2){
        return EXIT_FAILURE;
    }
    if (strcmp(flag, "se") != 0 && strcmp(flag, "sr") != 0 && strcmp(flag, "g0") != 0 &&
        strcmp(flag, "fg") != 0 && strcmp(flag, "fs") != 0){
        return EXIT_FAILURE;
    }
    char key[4*KB];
    strcpy(key, strtok(NULL, ":"));

    if (strcmp(flag, "se")==0||strcmp(flag, "sr")==0) {

        if (!valid_set(db, key)) {

            if (buf == send_again_buffer[send_again_index]){
                send_again_index++;
                return EXIT_SUCCESS;
            }
            else {
                send_again_buffer[send_again_index] = (char *) malloc(strlen(buf) + 1);
                send_again_kv_handle[send_again_index] = kv_handle;

                if (send_again_buffer[send_again_index] == NULL) {
                    return EXIT_FAILURE;
                }
                strcpy(send_again_buffer[send_again_index], buf);
                send_again_index++;
                return EXIT_SUCCESS;
            }
        }
        //////////////////////// Set Eager ////////////////////////
        if (strcmp(flag, "se") == 0) {
            printf("Eager set\n");
            char *value = strtok(NULL, ":");
            char *copy = (char *) malloc(strlen(value) + 1);
            if (copy == NULL) {
                return EXIT_FAILURE;
            }
            strcpy(copy, value);
            if (server_set_eager(db, kv_handle, key, copy) == EXIT_FAILURE) {
                return EXIT_FAILURE;
            }
        }

        //////////////////////// Set Rendezvous ////////////////////////
        else if (strcmp(flag, "sr") == 0) {
            printf("Rendezvous set\n");
            char *size = strtok(NULL, ":");
            int size_int = atoi(size);
            if (server_set_rendezvous(db, kv_handle, key, size_int) == EXIT_FAILURE) {
                return EXIT_FAILURE;
            }
        }
        if (buf == send_again_buffer[send_again_index]){
            free(send_again_buffer[send_again_index]);
        }
        return EXIT_SUCCESS;
    }

    //if FIN SET message
    else if (strcmp(flag, "fs")==0){
        printf("FIN SET\n");
        if (server_set_FIN(db, kv_handle, key) == EXIT_FAILURE){
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    else if (strcmp(flag, "g0")==0){
        Value* value;

        if (!valid_get(db, key)) {
            // now we want to add the buf to a buffer that saves the messages that we need to send again
            if (buf == send_again_buffer[send_again_index]){
                send_again_index++;
                return EXIT_SUCCESS;
            }
            else {
                send_again_buffer[send_again_index] = (char *) malloc(strlen(buf) + 1);
                send_again_kv_handle[send_again_index] = kv_handle;

                if (send_again_buffer[send_again_index] == NULL) {
                    return EXIT_FAILURE;
                }
                strcpy(send_again_buffer[send_again_index], buf);
                send_again_index++;
                return EXIT_SUCCESS;
            }
        }

        if (get_value(db, key, &value) == EXIT_FAILURE){
            return EXIT_FAILURE;
        }

        //////////////////////// Get Eager ////////////////////////
        if (value->is_large == false){
            printf("Eager get\n");
            if (server_get_eager(kv_handle, value) == EXIT_FAILURE){
                return EXIT_FAILURE;
            }
        }

            //////////////////////// Get Rendezvous ////////////////////////
        else{
            printf("Rendezvous get\n");
            if (server_get_rendezvous(db, kv_handle, value) == EXIT_FAILURE){
                return EXIT_FAILURE;
            }
        }
        if (buf == send_again_buffer[send_again_index]){
            free(send_again_buffer[send_again_index]);
        }
        return EXIT_SUCCESS;
    }
    //if FIN GET message
    else if (strcmp(flag, "fg")==0){
        printf("FIN GET\n");
        if (server_get_FIN(db, kv_handle, key) == EXIT_FAILURE){
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    return EXIT_SUCCESS;
}

int turn=0;
bool receive_query(Database* db, kvHandle** kv_handle){
    // get list of all the ctx
    struct pingpong_context* ctx_list[NUM_CLIENTS];
    kvHandle* handle;
    struct pingpong_context* ctx;
    for (int i = 0; i < NUM_CLIENTS; i++){
        handle = kv_handle[i];
        ctx = (struct pingpong_context*)handle;
        ctx_list[i] = ctx;
    }
    int client_index;
    char *buf;
    if ((turn % 2 == 0) && (send_again_index > 0)){
        //take old work from the buffer
        buf = send_again_buffer[send_again_index-1];
        for (int i = 0; i < NUM_CLIENTS; i++){
            if (kv_handle[i] == send_again_kv_handle[send_again_index-1]){
                client_index = i;
                break;
            }
        }

        send_again_index--;
    }
    else{
        // wait for completions
        if (pp_wait_completions_clients(ctx_list, 1,
                                        &client_index) == EXIT_FAILURE){
            return EXIT_FAILURE;
        }
        buf = ctx_list[client_index]->buf;
    }
    printf("Handling query from client %d\n", client_index);
    turn++;
    handle = kv_handle[client_index];
    ctx = (struct pingpong_context*)handle;

    // Parse data
    if (parse_data(db, handle, buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


////////////////////////// API Functions //////////////////////////

int kv_open(char *servername, void **kv_handle){
    struct pingpong_context** ctx_p = (struct pingpong_context**)kv_handle;
    if (init_connection(servername, ctx_p) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }
    printf("Opening connection to server %s\n", servername);
    return EXIT_SUCCESS;
}

int kv_set(void *kv_handle, const char *key, const char *value){
//    printf("Setting value for key: %s\n", key);
    // Flag = {get, set}
    if (strlen(value) < 4*KB-3){
        // send rendezvous control message
        if (client_set_eager(kv_handle, key, value) == EXIT_FAILURE){
            return EXIT_FAILURE;
        }
    }
    else{
        if (client_set_rendezvous(kv_handle, key, value) == EXIT_FAILURE){
            return EXIT_FAILURE;
        }
    }
    //send FIN SET message
    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;
    char* flag = "fs";
    sprintf(ctx.buf, "%s:%s:%c", flag, key,'\0');
    if (send_data_str(kv_handle, ctx.buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int kv_get(void *kv_handle, const char *key, char **value){
//    printf("Getting value for key: %s\n", key);

    struct pingpong_context ctx = *(struct pingpong_context*)kv_handle;
    // 1. send get message. Format: "g0:key\0"
    char* flag = "g0";
    sprintf(ctx.buf, "%s:%s:%c", flag, key,'\0');
    if (send_data_str(kv_handle, ctx.buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }

    // receive the value
    pp_wait_completions(&ctx, 1);
    char* buf = ctx.buf;
    if (buf[0] == 'e'){
        // copy without the flag in the form of "e:value"
        buf = buf + 2;
        *value = (char*)malloc(strlen(buf) + 1);
        sprintf(*value, "%s", buf);
        return EXIT_SUCCESS;
    }
    else{
        // Malloc size of the value. Format: "r:size"
        int size;
        sscanf(buf, "r:%d", &size);
        if (client_get_rendezvous(kv_handle, key, value, size) == EXIT_FAILURE){
            return EXIT_FAILURE;
        }
    }
    //send FIN GET message
    flag = "fg";
    sprintf(ctx.buf, "%s:%s:%c", flag, key,'\0');
    if (send_data_str(kv_handle, ctx.buf) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/* Called after get() on value pointer */
void kv_release(char *value){
    printf("Releasing value\n");
    free(value);
}

/* Destroys the QP */
int kv_close(void *kv_handle){
    struct pingpong_context* ctx = (struct pingpong_context*)kv_handle;
    if (pp_close_ctx(ctx) == EXIT_FAILURE){
        return EXIT_FAILURE;
    }
    printf("Closing connection\n");
    return EXIT_SUCCESS;
}