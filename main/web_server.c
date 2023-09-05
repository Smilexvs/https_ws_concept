#include <string.h>
#include <fcntl.h>
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "cJSON.h"

#include "web_server.h"

static const char *REST_TAG = "esp-rest";

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file( httpd_req_t *req, const char *filepath )
{
	const char *type = "text/plain";
	if( CHECK_FILE_EXTENSION( filepath, ".html" ) )
	{
		type = "text/html";
	}
	else if( CHECK_FILE_EXTENSION( filepath, ".js" ) )
	{
		type = "application/javascript";
	}
	else if( CHECK_FILE_EXTENSION( filepath, ".css" ) )
	{
		type = "text/css";
	}
	else if( CHECK_FILE_EXTENSION( filepath, ".png" ) )
	{
		type = "image/png";
	}
	else if( CHECK_FILE_EXTENSION( filepath, ".ico" ) )
	{
		type = "image/x-icon";
	}
	else if( CHECK_FILE_EXTENSION( filepath, ".svg" ) )
	{
		type = "text/xml";
	}
	return httpd_resp_set_type( req, type );
}

/* Send HTTP response with the contents of the requested file */
esp_err_t rest_common_get_handler( httpd_req_t *req )
{
	char filepath[FILE_PATH_MAX];

	rest_server_context_t *rest_context = (rest_server_context_t*) req->user_ctx;
	strlcpy( filepath, rest_context->base_path, sizeof(filepath) );
	if( req->uri[strlen( req->uri ) - 1] == '/' )
	{
		strlcat( filepath, "/index.html", sizeof(filepath) );
	}
	else
	{
		strlcat( filepath, req->uri, sizeof(filepath) );
	}
	int fd = open( filepath, O_RDONLY, 0 );
	if( fd == -1 )
	{
		ESP_LOGE( REST_TAG, "Failed to open file : %s", filepath );
		/* Respond with 500 Internal Server Error */
		httpd_resp_send_err( req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file" );
		return ESP_FAIL;
	}

	set_content_type_from_file( req, filepath );

	char *chunk = rest_context->scratch;
	ssize_t read_bytes;
	do
	{
		/* Read file in chunks into the scratch buffer */
		read_bytes = read( fd, chunk, SCRATCH_BUFSIZE );
		if( read_bytes == -1 )
		{
			ESP_LOGE( REST_TAG, "Failed to read file : %s", filepath );
		}
		else if( read_bytes > 0 )
		{
			/* Send the buffer contents as HTTP response chunk */
			if( httpd_resp_send_chunk( req, chunk, read_bytes ) != ESP_OK )
			{
				close( fd );
				ESP_LOGE( REST_TAG, "File sending failed!" );
				/* Abort sending file */
				httpd_resp_sendstr_chunk( req, NULL );
				/* Respond with 500 Internal Server Error */
				httpd_resp_send_err( req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file" );
				return ESP_FAIL;
			}
		}
	}while( read_bytes > 0 );
	/* Close file after sending complete */
	close( fd );
	ESP_LOGI( REST_TAG, "File sending complete" );
	/* Respond with an empty chunk to signal HTTP response completion */
	httpd_resp_send_chunk( req, NULL, 0 );
	return ESP_OK;
}

bool sendData = false;

esp_err_t ws_process_data( const uint8_t *payload )
{
	cJSON *object = cJSON_Parse( (char*) payload );
	const cJSON *command;
	if( NULL == object )
	{
		return ESP_FAIL;
	}
	command = cJSON_GetObjectItemCaseSensitive( object, "command" );
	if( NULL == command )
	{
		cJSON_Delete( object );
		return ESP_FAIL;
	}

	if( 0 == strcmp( command->valuestring, "start" ) )
	{
		ESP_LOGI( REST_TAG, "command START" );
		sendData = true;
	}
	else if( 0 == strcmp( command->valuestring, "stop" ) )
	{
		ESP_LOGI( REST_TAG, "command STOP" );
		sendData = false;
	}
	else
	{
		ESP_LOGI( REST_TAG, "command UNKNOWN" );
	}
	cJSON_Delete( object );
	return ESP_OK;
}

#define PATIENTS_COUNT		6
#define JSON_BUFFER_SIZE	2048
static char jsonBuffer[JSON_BUFFER_SIZE];

static const char *patientNameTable[] =
{
 "Peter", "Alex", "Emma", "Max", "Sam", "Dave"
};

void send_data_task( httpd_handle_t server )
{
	httpd_ws_frame_t ws_pkt;

	cJSON *mainJson;
	cJSON *arrayJson;
	cJSON *curJson;
	cJSON *cutItem;

	while( true )
	{
		if( sendData )
		{
			mainJson = cJSON_CreateObject();
			if( mainJson == NULL )
			{
				goto printEnd;
			}
			arrayJson = cJSON_AddArrayToObject( mainJson, "patient" );
			if( arrayJson == NULL )
			{
				goto printEnd;
			}

			for( size_t i = 0; i < PATIENTS_COUNT; i++ )
			{
				curJson = cJSON_CreateObject();
				JSON_ADD_STRING( curJson, cutItem, "name", patientNameTable[i] );
				JSON_ADD_NUMBER( curJson, cutItem, "temperature", 36.6 );
				JSON_ADD_STRING( curJson, cutItem, "bloodPressure", "120/70" );
				JSON_ADD_NUMBER( curJson, cutItem, "heartRate", 60 + (random() % 60) );

				if( false == cJSON_AddItemToArray( arrayJson, curJson ) )
				{
					goto printEnd;
				}
			}

			cJSON_PrintPreallocated( mainJson, jsonBuffer, JSON_BUFFER_SIZE,
			false );

			size_t clients = 4;
			int client_fds[4];
			if( httpd_get_client_list( server, &clients, client_fds ) == ESP_OK )
			{
				for( size_t i = 0; i < clients; ++i )
				{
					int sock = client_fds[i];
					if( httpd_ws_get_fd_info( server, sock ) == HTTPD_WS_CLIENT_WEBSOCKET )
					{
						ws_pkt.payload = (uint8_t*) jsonBuffer;
						ws_pkt.len = strlen( jsonBuffer );
						ws_pkt.type = HTTPD_WS_TYPE_TEXT;

						httpd_ws_send_frame_async( server, sock, &ws_pkt );
					}
				}
			}
			else
			{
				ESP_LOGE( REST_TAG, "httpd_get_client_list failed!" );
				return;
			}

printEnd:
			cJSON_Delete( mainJson );
		}

		vTaskDelay( 1000 / portTICK_PERIOD_MS );
	}

}

