#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs_semihost.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "esp_wifi.h"

#include "esp_tls.h"

#include "web_server.h"
#include "keep_alive.h"

#define MDNS_INSTANCE 	"esp web server"
#define MDNS_HOST_NAME	"ESP-HTTPS"

#define BASE_PATH		"/spiffs"

static const char *TAG = "https-wss";

static const size_t max_clients = 4;

static void initialise_mdns( void )
{
	mdns_init();
	mdns_hostname_set( MDNS_HOST_NAME );
	mdns_instance_name_set( MDNS_INSTANCE );

	mdns_txt_item_t serviceTxtData[] =
	{
	{ "board", "esp32" },
	{ "path", "/" } };

	ESP_ERROR_CHECK(
			mdns_service_add( "ESP32-WebServer", "_https", "_tcp", 443, serviceTxtData,
					sizeof(serviceTxtData) / sizeof(serviceTxtData[0]) ) );
}

esp_err_t init_semi_fs( void )
{
	esp_err_t ret = esp_vfs_semihost_register( BASE_PATH );
	if( ret != ESP_OK )
	{
		ESP_LOGE( TAG, "Failed to register semihost driver (%s)!", esp_err_to_name( ret ) );
		return ESP_FAIL;
	}
	return ESP_OK;
}

esp_err_t init_fs( void )
{
	esp_vfs_spiffs_conf_t conf =
	{ .base_path = BASE_PATH, .partition_label = NULL, .max_files = 5, .format_if_mount_failed = false };
	esp_err_t ret = esp_vfs_spiffs_register( &conf );

	if( ret != ESP_OK )
	{
		if( ret == ESP_FAIL )
		{
			ESP_LOGE( TAG, "Failed to mount or format filesystem" );
		}
		else if( ret == ESP_ERR_NOT_FOUND )
		{
			ESP_LOGE( TAG, "Failed to find SPIFFS partition" );
		}
		else
		{
			ESP_LOGE( TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name( ret ) );
		}
		return ESP_FAIL;
	}

	size_t total = 0, used = 0;
	ret = esp_spiffs_info( NULL, &total, &used );
	if( ret != ESP_OK )
	{
		ESP_LOGE( TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name( ret ) );
	}
	else
	{
		ESP_LOGI( TAG, "Partition size: total: %d, used: %d", total, used );
	}
	return ESP_OK;
}

/* An HTTP GET handler */
rest_server_context_t rest_context;
extern esp_err_t rest_common_get_handler( httpd_req_t *req );

esp_err_t wss_open_fd( httpd_handle_t hd, int sockfd )
{
	ESP_LOGI( TAG, "New client connected %d", sockfd );
	wss_keep_alive_t h = httpd_get_global_user_ctx( hd );
	return wss_keep_alive_add_client( h, sockfd );
}

void wss_close_fd( httpd_handle_t hd, int sockfd )
{
	ESP_LOGI( TAG, "Client disconnected %d", sockfd );
	wss_keep_alive_t h = httpd_get_global_user_ctx( hd );
	wss_keep_alive_remove_client( h, sockfd );
	close( sockfd );
}

static esp_err_t ws_handler( httpd_req_t *req )
{
	if( req->method == HTTP_GET )
	{
		ESP_LOGI( TAG, "Handshake done, the new connection was opened" );
		return ESP_OK;
	}
	httpd_ws_frame_t ws_pkt;
	uint8_t *buf = NULL;
	memset( &ws_pkt, 0, sizeof(httpd_ws_frame_t) );

	// First receive the full ws message
	/* Set max_len = 0 to get the frame len */
	esp_err_t ret = httpd_ws_recv_frame( req, &ws_pkt, 0 );
	if( ret != ESP_OK )
	{
		ESP_LOGE( TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret );
		return ret;
	}

	if( ws_pkt.len )
	{
		/* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
		buf = calloc( 1, ws_pkt.len + 1 );
		if( buf == NULL )
		{
			ESP_LOGE( TAG, "Failed to calloc memory for buf" );
			return ESP_ERR_NO_MEM;
		}
		ws_pkt.payload = buf;
		/* Set max_len = ws_pkt.len to get the frame payload */
		ret = httpd_ws_recv_frame( req, &ws_pkt, ws_pkt.len );
		if( ret != ESP_OK )
		{
			ESP_LOGE( TAG, "httpd_ws_recv_frame failed with %d", ret );
			free( buf );
			return ret;
		}
	}
	// If it was a PONG, update the keep-alive
	if( ws_pkt.type == HTTPD_WS_TYPE_PONG )
	{
		ESP_LOGD( TAG, "Received PONG message" );
		free( buf );
		return wss_keep_alive_client_is_active( httpd_get_global_user_ctx( req->handle ), httpd_req_to_sockfd( req ) );

		// If it was a TEXT message, just echo it back
	}
	else if( ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_PING
			|| ws_pkt.type == HTTPD_WS_TYPE_CLOSE )
	{
		if( ws_pkt.type == HTTPD_WS_TYPE_TEXT )
		{
			ws_process_data( ws_pkt.payload );
		}
		else if( ws_pkt.type == HTTPD_WS_TYPE_PING )
		{
			// Response PONG packet to peer
			ESP_LOGI( TAG, "Got a WS PING frame, Replying PONG" );
			ws_pkt.type = HTTPD_WS_TYPE_PONG;
		}
		else if( ws_pkt.type == HTTPD_WS_TYPE_CLOSE )
		{
			// Response CLOSE packet with no payload to peer
			ws_pkt.len = 0;
			ws_pkt.payload = NULL;
		}
		ret = httpd_ws_send_frame( req, &ws_pkt );
		if( ret != ESP_OK )
		{
			ESP_LOGE( TAG, "httpd_ws_send_frame failed with %d", ret );
		}
		ESP_LOGI( TAG, "ws_handler: httpd_handle_t=%p, sockfd=%d, client_info:%d", req->handle,
				httpd_req_to_sockfd( req ), httpd_ws_get_fd_info( req->handle, httpd_req_to_sockfd( req ) ) );
		free( buf );
		return ret;
	}
	free( buf );
	return ESP_OK;
}

static const httpd_uri_t ws =
		{ .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .user_ctx = NULL, .is_websocket = true, .handle_ws_control_frames =
					true };

static void send_ping( void *arg )
{
	struct async_resp_arg *resp_arg = arg;
	httpd_handle_t hd = resp_arg->hd;
	int fd = resp_arg->fd;
	httpd_ws_frame_t ws_pkt;
	memset( &ws_pkt, 0, sizeof(httpd_ws_frame_t) );
	ws_pkt.payload = NULL;
	ws_pkt.len = 0;
	ws_pkt.type = HTTPD_WS_TYPE_PING;

	httpd_ws_send_frame_async( hd, fd, &ws_pkt );
	free( resp_arg );
}

bool client_not_alive_cb( wss_keep_alive_t h, int fd )
{
	ESP_LOGE( TAG, "Client not alive, closing fd %d", fd );
	httpd_sess_trigger_close( wss_keep_alive_get_user_ctx( h ), fd );
	return true;
}

bool check_client_alive_cb( wss_keep_alive_t h, int fd )
{
	ESP_LOGD( TAG, "Checking if client (fd=%d) is alive", fd );
	struct async_resp_arg *resp_arg = malloc( sizeof(struct async_resp_arg) );
	resp_arg->hd = wss_keep_alive_get_user_ctx( h );
	resp_arg->fd = fd;

	if( httpd_queue_work( resp_arg->hd, send_ping, resp_arg ) == ESP_OK )
	{
		return true;
	}
	return false;
}

static httpd_handle_t start_webserver( void )
{
	// Prepare keep-alive engine
	wss_keep_alive_config_t keep_alive_config = KEEP_ALIVE_CONFIG_DEFAULT();
	keep_alive_config.max_clients = max_clients;
	keep_alive_config.client_not_alive_cb = client_not_alive_cb;
	keep_alive_config.check_client_alive_cb = check_client_alive_cb;
	wss_keep_alive_t keep_alive = wss_keep_alive_start( &keep_alive_config );

	// Start the httpd server
	httpd_handle_t server = NULL;
	ESP_LOGI( TAG, "Starting server" );

	httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
	conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
	conf.httpd.max_open_sockets = max_clients;
	conf.httpd.global_user_ctx = keep_alive;
	conf.httpd.open_fn = wss_open_fd;
	conf.httpd.close_fn = wss_close_fd;

	extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
	extern const unsigned char servercert_end[] asm("_binary_servercert_pem_end");
	conf.servercert = servercert_start;
	conf.servercert_len = servercert_end - servercert_start;

	extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
	extern const unsigned char prvtkey_pem_end[] asm("_binary_prvtkey_pem_end");
	conf.prvtkey_pem = prvtkey_pem_start;
	conf.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

	esp_err_t ret = httpd_ssl_start( &server, &conf );
	if( ESP_OK != ret )
	{
		ESP_LOGI( TAG, "Error starting server!" );
		return NULL;
	}

	// Set URI handlers
	ESP_LOGI( TAG, "Registering URI handlers" );
	/* URI handler for getting web server files */
	httpd_register_uri_handler( server, &ws );
	wss_keep_alive_set_user_ctx( keep_alive, server );

	strcpy( rest_context.base_path, "/spiffs" );
	httpd_uri_t common_get_uri =
	{ .uri = "/*", .method = HTTP_GET, .handler = rest_common_get_handler, .user_ctx = &rest_context };
	httpd_register_uri_handler( server, &common_get_uri );

	return server;
}

void app_main( void )
{
	wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
	wifi_config_t wifi_config =
	{ .sta =
	{ .ssid = CONFIG_ESP_WIFI_SSID, .password = CONFIG_ESP_WIFI_PASSWORD, .threshold.authmode = WIFI_AUTH_WPA2_PSK, }, };

	ESP_ERROR_CHECK( nvs_flash_init() );
	ESP_ERROR_CHECK( esp_netif_init() );
	ESP_ERROR_CHECK( esp_event_loop_create_default() );
	initialise_mdns();
	netbiosns_init();
	netbiosns_set_name( MDNS_HOST_NAME );

	ESP_ERROR_CHECK( init_fs() );

	esp_netif_create_default_wifi_sta();
	ESP_ERROR_CHECK( esp_wifi_init( &wifi_init_config ) );
	ESP_ERROR_CHECK( esp_wifi_set_mode( WIFI_MODE_STA ) );
	ESP_ERROR_CHECK( esp_wifi_set_config( WIFI_IF_STA, &wifi_config ) );
	ESP_ERROR_CHECK( esp_wifi_start() );
	ESP_ERROR_CHECK( esp_wifi_connect() );

	httpd_handle_t server = start_webserver();

	send_data_task( server );
}
