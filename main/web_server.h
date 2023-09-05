#ifndef MAIN_WEB_SERVER_H_
#define MAIN_WEB_SERVER_H_

#include <esp_https_server.h>

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

typedef struct rest_server_context {
    char base_path[FILE_PATH_MAX];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

esp_err_t ws_process_data( const uint8_t *payload );
void send_data_task( httpd_handle_t server );

#define JSON_ADD_STRING( parent, child, childName, childString ) \
	child = cJSON_CreateString( childString ); \
	if( NULL == child ) \
	{ \
		goto printEnd; \
	} \
	cJSON_AddItemToObject( parent, childName, child );

#define JSON_ADD_NUMBER( parent, child, childName, childNum ) \
	child = cJSON_CreateNumber( childNum ); \
	if( NULL == child ) \
	{ \
		goto printEnd; \
	} \
	cJSON_AddItemToObject( parent, childName, child );

#define JSON_ADD_BOOL( parent, child, childName, childBool ) \
	child = cJSON_CreateBool( childBool ); \
	if( NULL == child ) \
	{ \
		goto printEnd; \
	} \
	cJSON_AddItemToObject( parent, childName, child );

#endif /* MAIN_WEB_SERVER_H_ */
