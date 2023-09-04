#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "esp_wifi.h"
#include <esp_https_server.h>
#include "esp_tls.h"

#define MDNS_INSTANCE 	"esp web server"
#define MDNS_HOST_NAME	"ESP-HTTPS"

static const char *TAG = "https-wss";

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

esp_err_t init_fs( void )
{
	esp_vfs_spiffs_conf_t conf =
	{ .base_path = "/spiffs", .partition_label = NULL, .max_files = 5, .format_if_mount_failed = false };
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
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h1>Hello Secure World!</h1>", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();

    extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_end[]   asm("_binary_servercert_pem_end");
    conf.servercert = servercert_start;
    conf.servercert_len = servercert_end - servercert_start;

    extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
    extern const unsigned char prvtkey_pem_end[]   asm("_binary_prvtkey_pem_end");
    conf.prvtkey_pem = prvtkey_pem_start;
    conf.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ESP_OK != ret) {
        ESP_LOGI(TAG, "Error starting server!");
        return NULL;
    }

    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    httpd_register_uri_handler(server, &root);
    return server;
}

void app_main( void )
{
	wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
	wifi_config_t wifi_config =
	{ .sta =
	{ .ssid = CONFIG_ESP_WIFI_SSID, .password = CONFIG_ESP_WIFI_PASSWORD,
	.threshold.authmode = WIFI_AUTH_WPA2_PSK, }, };

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

	start_webserver();

	while( true )
	{
		printf( "Hello from app_main!\n" );
		sleep( 1 );
	}
}
