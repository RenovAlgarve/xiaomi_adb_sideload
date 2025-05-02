#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <libusb.h>
#include <curl/curl.h>
#include <AES/aes.h>
#include <JSON/tiny-json.h>
#include <BASE64/base64.h>
#include <MD5/md5.h>

#define ADB_CLASS 0xff
#define ADB_SUB_CLASS 0x42
#define ADB_PROTOCOL_CODE 1
#define ADB_CONNECT 0x4E584E43
#define ADB_VERSION 0x01000001
#define ADB_OPEN 0x4E45504F
#define ADB_OKAY 0x59414B4F
#define ADB_WRTE 0x45545257
#define ADB_CLSE 0x45534C43
#define ADB_MAX_DATA 1024 *1024 //1 MB
#define CHUNK_SIZE 2048
#define ADB_SIDELOAD_CHUNK_SIZE 1024 *64

libusb_context *ctx;
libusb_device_handle *dev_handle;

int bulk_in;
int bulk_out;
int interface_num;

char* codename;
char* version;
char* serial_num;
char* codebase;
char* branch;
char* lang;
char* region;
char* romzone;

typedef struct {
 uint32_t cmd;
 uint32_t arg0;
 uint32_t arg1;
 uint32_t len;
 uint32_t checksum;
 uint32_t magic;
} adb_usb_packet;

typedef struct {
 char *buffer;
 int len;
 int buflen;
} get_request;

static bool endpoint_is_output(uint8_t endpoint) {
 return (endpoint & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT;
}

void printUsage(char* exename) {
 printf("Usage: %s [option] [arg]\n", exename);
 printf("--format-data\t\t\t Format userdata in sideload\n");
 printf("--sideload [filename]\t\t Flash recovery OTA in sideload\n");
 printf("--generate-sign [filename]\t Generate sign file for flashing OTA, provide firmware\n");
}

int check_device(libusb_device *dev) {

 struct libusb_device_descriptor desc;
 int r = libusb_get_device_descriptor(dev, &desc);
 if (r != LIBUSB_SUCCESS) {
 printf("Failed to get device descriptor\n");
 return 1;
 }

 struct libusb_config_descriptor *configs;
 r = libusb_get_active_config_descriptor(dev, &configs);
 if (r != LIBUSB_SUCCESS) {
 printf("Failed to get active config descriptor\n");
 return 1;
 }

 bulk_in = -1;
 bulk_out = -1;
 interface_num = -1;

 for (int i =0; i < configs->bNumInterfaces; i++) {
 struct libusb_interface intf = configs->interface[i];
 if (intf.num_altsetting ==0) {
 continue;
 }

 interface_num = i;
 struct libusb_interface_descriptor intf_desc = intf.altsetting[0];
 if (!(intf_desc.bInterfaceClass == ADB_CLASS && intf_desc.bInterfaceSubClass == ADB_SUB_CLASS && intf_desc.bInterfaceProtocol == ADB_PROTOCOL_CODE)) {
 continue;
 }

 if (intf.num_altsetting !=1) {
 continue;
 }

 for(int endpoint_num =0; endpoint_num < intf_desc.bNumEndpoints; endpoint_num++) {
 struct libusb_endpoint_descriptor ep = intf_desc.endpoint[endpoint_num];
 const uint8_t endpoint_addr = ep.bEndpointAddress;
 const uint8_t endpoint_attr = ep.bmAttributes;
 const uint8_t transfer_type = endpoint_attr & LIBUSB_TRANSFER_TYPE_MASK;

 if (transfer_type != LIBUSB_TRANSFER_TYPE_BULK) {
 continue;
 }

 if (endpoint_is_output(endpoint_addr) && bulk_out == -1) {
 bulk_out = endpoint_addr;
 } else if (!endpoint_is_output(endpoint_addr) && bulk_in == -1) {
 bulk_in = endpoint_addr;
 }

 if(bulk_out != -1 && bulk_in != -1) {
 return 0;
 }
 }
 }

 return 1;
}

int scan_for_device_from_fd(int fd) {
 int r = libusb_init(&ctx);
 if (r != LIBUSB_SUCCESS) {
 printf("Failed to init libusb\n");
 return 1;
 }
 libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
 libusb_wrap_sys_device(ctx, (intptr_t) fd, &dev_handle);
 libusb_device *dev = libusb_get_device(dev_handle);

 if(check_device(dev) ==0) {   
 r = libusb_claim_interface(dev_handle, interface_num);
 if(r != LIBUSB_SUCCESS) {
 printf("Failed to claim interface\n");
 return 1;
 }
 }
 return 0;
}

int scan_for_device() {
 int r = libusb_init(&ctx);
 if (r != LIBUSB_SUCCESS) {
 printf("Failed to init libusb\n");
 return 1;
 }

 libusb_device **devs= NULL;
 libusb_device *dev= NULL;
 ssize_t cnt = libusb_get_device_list(ctx, &devs);
 if (cnt <0) {
 printf("Failed to get device list\n");
 return 1;
 }

 int i =0;
 bool found = false;
 while ((dev = devs[i++]) != NULL) {
 if(check_device(dev) ==0) {   
 found = true;
 r = libusb_open(dev, &dev_handle);
 if(r != LIBUSB_SUCCESS) {
 printf("Failed to open usb device\n");
 return 1;
 }
 r = libusb_claim_interface(dev_handle, interface_num);
 if(r != LIBUSB_SUCCESS) {
 printf("Failed to claim interface\n");
 return 1;
 }
 break;
 }
 }

 libusb_free_device_list(devs,1);
    
 if (found) {
 return 0;
 } else {
 return 1;
 };
}

int usb_read(void *data, int datalen) {
 int read_len;
 int r = libusb_bulk_transfer(dev_handle, bulk_in, data, datalen, &read_len,1000);
 if (r != LIBUSB_SUCCESS) {
 printf("usb bulk read error\n");
 return -1;
 }
 return read_len;
}

int usb_write(void *data, int datalen) {
 int write_len;
 int r = libusb_bulk_transfer(dev_handle, bulk_out, data, datalen, &write_len,1000);
 if (r != LIBUSB_SUCCESS) {
 printf("usb bulk write error\n");
 return -1;
 }
 return write_len;
}

int send_command(uint32_t cmd, uint32_t arg0, uint32_t arg1, void *data, int datalen) {
 adb_usb_packet pkt;
 pkt.cmd = cmd;
 pkt.arg0 = arg0;
 pkt.arg1 = arg1;
 pkt.len = datalen;
 pkt.checksum =0;
 pkt.magic = cmd ^0xffffffff;

 if(usb_write(&pkt, sizeof(pkt)) == -1) {
 return 1;
 } 

 if(datalen >0) {
 if(usb_write(data, datalen) == -1) {
 return 1;
 }
 }
 return 0;
}

int recv_packet(adb_usb_packet *pkt, void* data, int *data_len) {
 if(!usb_read(pkt, sizeof(adb_usb_packet))) {
 return 1;
 }

 if(pkt->len >0) {
 if(!usb_read(data, pkt->len)) {
 return 1;
 }
 }

 *data_len = pkt->len;
 return 0;
}

int send_recovery_commands(char* command, char* response) {
 int cmd_len = strlen(command);
 char cmd[cmd_len +1];
 memcpy(cmd, command, cmd_len);
 cmd_len++;
 cmd[cmd_len] =0;

 if(send_command(ADB_OPEN,1,0, cmd, cmd_len)) {
 printf("device not accept connect request\n");
 return 1;
 }

 adb_usb_packet pkt;
 char data[512];
 int data_len;
 recv_packet(&pkt, data, &data_len); 

 if(recv_packet(&pkt, response, &data_len)) {
 printf("Failed to get info from device\n");
 return 1;
 }

 response[data_len] =0;
 if(response[data_len -1] == '\n')
 response[data_len -1] =0;

 recv_packet(&pkt, data, &data_len); 
 return 0;

}

int connect_device_read_info(bool read_info) {
 if(send_command(ADB_CONNECT, ADB_VERSION, ADB_MAX_DATA, "host::\x0",7)) {
 printf("device not accept connect request\n");
 return 1;
 }

 char buf[512];
 int buf_len;
 adb_usb_packet pkt;
 int try_count =10;
 while (try_count >0) {
 if(recv_packet(&pkt, buf, &buf_len)) {
 printf("Failed to read response from device\n");
 return 1;
 }
 if(pkt.cmd == ADB_CONNECT) break;
 try_count--;
 }

 if(try_count ==0) {
 printf("Device doesn't send correct response\n");
 return 1;
 }

 buf[buf_len] =0;
 if(memcmp(buf, "sideload::",10)){
 return 1;
 }

 if(!read_info) {
 return 0;
 }

 if(send_recovery_commands("getdevice:", codename)) {
 printf("Failed to execute getdevice");
 return 1;
 }

 if(send_recovery_commands("getversion:", version)) {
 printf("Failed to execute getdevice");
 return 1;
 }

 if(send_recovery_commands("getsn:", serial_num)) {
 printf("Failed to execute getdevice");
 return 1;
 }

 if(send_recovery_commands("getcodebase:", codebase)) {
 printf("Failed to execute getdevice");
 return 1;
 }

 if(send_recovery_commands("getbranch:", branch)) {
 printf("Failed to execute getdevice");
 return 1;
 }

 if(send_recovery_commands("getlanguage:", lang)) {
 printf("Failed to execute getdevice");
 return 1;
 }

 if(send_recovery_commands("getregion:", region)) {
 printf("Failed to execute getdevice");
 return 1;
 }

 if(send_recovery_commands("getromzone:", romzone)) {
 printf("Failed to execute getdevice");
 return 1;
 }

 return 0;
}

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
 size_t realsize = size * nmemb; 
 get_request *req = (get_request *) userdata;
 while (req->buflen < req->len + realsize +1) {
 req->buffer = realloc(req->buffer, req->buflen + CHUNK_SIZE);
 req->buflen += CHUNK_SIZE;
 }
 memcpy(&req->buffer[req->len], ptr, realsize);
 req->len += realsize;
 req->buffer[req->len] =0;

 return realsize;
}

int fileexists(const char *fname) {
 FILE *file;
 if ((file = fopen(fname, "r"))) {
 fclose(file);
 return 1;
 }
 return 0;
}

char* generate_md5_hash(char* filename) {
 FILE *fp = fopen(filename, "r");
 if (fp == NULL) {
 printf("Failed to open file\n");
 return NULL;
 }

 uint8_t hash[16];
 md5File(fp, hash);
 fclose(fp);

 char *og_ptr = malloc(32);
 if (og_ptr == NULL) {
 printf("Failed to allocate memory\n");
 return NULL;
 }

 char *ptr = og_ptr;
 for(int i =0; i <16; i++) {
 ptr += sprintf(ptr, "%02x", hash[i]);
 }
 return og_ptr;
}

int generate_firmware_sign(char* signfile) {
    const uint8_t key[16] = { 0x6D, 0x69, 0x75, 0x69, 0x6F, 0x74, 0x61, 0x76, 0x61, 0x6C, 0x69, 0x64, 0x65, 0x64, 0x31, 0x31};
    const uint8_t iv[16] = { 0x30, 0x31, 0x30, 0x32, 0x30, 0x33, 0x30, 0x34, 0x30, 0x35, 0x30, 0x36, 0x30, 0x37, 0x30, 0x38};

    if (!fileexists(signfile)) {
        printf("Firmware file not found: %s\n", signfile);
        return 1;
    }

    char* pkg_hash = generate_md5_hash(signfile);
    if (!pkg_hash) {
        printf("Failed to generate MD5 hash\n");
        return 1;
    }

    char raw_json[2048];
    snprintf(raw_json, sizeof(raw_json),
        "{\n\t\"d\" : \"%s\",\n\t\"v\" : \"%s\",\n\t\"c\" : \"%s\",\n\t\"b\" : \"%s\",\n\t\"sn\" : \"%s\",\n\t\"r\" : \"GL\",\n\t\"l\" : \"en-US\",\n\t\"f\" : \"1\",\n\t\"id\" : \"\",\n\t\"options\" : {\n\t\t\"zone\" : %s\n\t},\n\t\"pkg\" : \"%s\"\n}",
        codename, version, codebase, branch, serial_num, romzone, pkg_hash
    );
    free(pkg_hash);

    int len = strlen(raw_json);
    int padded_len = len + (16 - (len % 16));
    char *padded_json = malloc(padded_len);
    if (!padded_json) {
        printf("Memory alloc fail\n");
        return 1;
    }
    memcpy(padded_json, raw_json, len);

    uint8_t pad = 16 - (len % 16);
    for (int i = 0; i < pad; i++) {
        padded_json[len + i] = pad;
    }

    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv);
    AES_CBC_encrypt_buffer(&ctx, (uint8_t *)padded_json, padded_len);

    int b64_len = b64_encodedLength(padded_len);
    char *b64_buf = malloc(b64_len + 1);
    if (!b64_buf) {
        free(padded_json);
        return 1;
    }
    b64_encode((uint8_t *)padded_json, padded_len, (uint8_t *)b64_buf);
    b64_buf[b64_len] = 0;
    free(padded_json);

    curl_global_init(CURL_GLOBAL_ALL);
    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("CURL init failed\n");
        free(b64_buf);
        return 1;
    }

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "clientId: MITUNES");
    headers = curl_slist_append(headers, "Connection: Keep-Alive");
    headers = curl_slist_append(headers, "Accept-Encoding: identity");
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    char *json_post_data = curl_easy_escape(curl, b64_buf, strlen(b64_buf));
    free(b64_buf);

    char *post_buf = malloc(4096);
    if (!post_buf) {
        curl_free(json_post_data);
        curl_easy_cleanup(curl);
        return 1;
    }

    snprintf(post_buf, 4096, "q=%s&t=&s=1", json_post_data);
    get_request req = {.buffer = malloc(CHUNK_SIZE), .len = 0, .buflen = CHUNK_SIZE};
    if (!req.buffer) {
        free(post_buf);
        curl_free(json_post_data);
        curl_easy_cleanup(curl);
        return 1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, "http://update.miui.com/updates/miotaV3.php");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MiTunes_UserAgent_v3.0");
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_buf);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &req);

    CURLcode res = curl_easy_perform(curl);
    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    int result = 1;

    if (res == CURLE_OK && status_code == 200) {
        int dec_len;
        curl_free(json_post_data);
        json_post_data = curl_easy_unescape(curl, req.buffer, req.len, &dec_len);

        char *dec_buf = malloc(dec_len + 1);
        memset(dec_buf, 0, dec_len + 1);
        b64_decode((uint8_t *)json_post_data, dec_len, (uint8_t *)dec_buf);

        AES_init_ctx_iv(&ctx, key, iv);
        AES_CBC_decrypt_buffer(&ctx, (uint8_t *)dec_buf, dec_len);

        uint8_t unpad = dec_buf[dec_len - 1];
        if (unpad > 16) goto cleanup;

        dec_buf[dec_len - unpad] = 0;

        json_t mem[64];
        const json_t *json = json_create(dec_buf, mem, sizeof mem / sizeof *mem);
        if (!json) {
            printf("JSON parse failed\n");
            goto cleanup;
        }

        const json_t *pkgRom = json_getProperty(json, "PkgRom");
        if (!pkgRom) {
            printf("Missing PkgRom field\n");
            goto cleanup;
        }

        const char *validate = json_getPropertyValue(pkgRom, "Validate");
        if (!validate) {
            printf("Missing Validate field\n");
            goto cleanup;
        }

        FILE *fp = fopen("validate.key", "w");
        fwrite(validate, 1, strlen(validate), fp);
        fclose(fp);
        printf("Sign generated and saved to validate.key\n");
        result = 0;

    } else {
        printf("Server error or CURL failure: %s\n", curl_easy_strerror(res));
    }

cleanup:
    curl_free(json_post_data);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(post_buf);
    free(req.buffer);

    return result;
}

int start_sideload(const char *sideload_file) {
    
 FILE *fp = fopen("validate.key", "r");
 if (fp == NULL) {
 printf("Failed to open file\n");
 return 1;
 }

 fseek(fp,0, SEEK_END);
 long validate_file_size = ftell(fp);
 fseek(fp,0, SEEK_SET);
 char *validate = malloc(validate_file_size +1);
 if (validate == NULL) {
 printf("Failed to allocate memory\n");
 fclose(fp);
 return 1;
 }

 fread(validate,1, validate_file_size, fp);
 fclose(fp);
 validate[validate_file_size] =0;

 fp = fopen(sideload_file, "r");
 if (fp == NULL) {
 printf("Failed to open file\n");
 free(validate);
 return 1;
 }

 fseek(fp,0, SEEK_END);
 long file_size = ftell(fp);
 char sideload_host_command[128 + validate_file_size];
 memset(sideload_host_command,0,128 + validate_file_size);
 sprintf(sideload_host_command, "sideload-host:%ld:%d:%s:0", file_size, ADB_SIDELOAD_CHUNK_SIZE, validate);
 free(validate);

 send_command(ADB_OPEN,1,0, sideload_host_command, strlen(sideload_host_command) +1);

 uint8_t *work_buffer = malloc(ADB_SIDELOAD_CHUNK_SIZE);
 if (work_buffer == NULL) {
 printf("Failed to allocate memory\n");
 return 1;
}
}

int main(int argc, char** argv) {
 int opt;
 bool format_data = false;
 bool generate_sign = false;
 char* sideloadfile = NULL;
 char* signfile = NULL;

 static struct option options[] = {
 {"format-data", no_argument,0, 'f'},
 {"sideload", required_argument,0, 's'},
 {"generate-sign", required_argument,0, 'g'},
 {0,0,0,0}
 };

 while ((opt = getopt_long(argc, argv, "fsg", options, NULL )) != -1) {
	switch (opt) {
	case 'f':
	format_data = true;
	break;
	case 's':
	sideloadfile = optarg;
	break;
 case 'g':
 generate_sign = true;
 signfile = optarg;
 break;
	default:
	printUsage(argv[0]);
	return 1;

 if(optind ==1) {
 printUsage(argv[0]);
 return 1;
 }

 codename = (char *)malloc(64);
 version = (char *)malloc(64);
 serial_num = (char *)malloc(64);
 codebase = (char *)malloc(64);
 branch = (char *)malloc(64);
 lang = (char *)malloc(64);
 region = (char *)malloc(64);
 romzone = (char *)malloc(64);

 // quick hack to use TERMUX-USB 
 char *fd_s = getenv("TERMUX_USB_FD");
 if(fd_s != NULL) {
 int fd = atoi(fd_s);
 if(scan_for_device_from_fd(fd)) {
 printf("Incorrect device\n");
 return 0; 
 }
 } else {
 if(scan_for_device() !=0) {
 printf("No device found\n");
 return 0;
 }
 }

 bool connection = true;
 bool readinfo = true;
 if(sideloadfile != NULL) {
 readinfo = false;
 }

 if(connect_device_read_info(readinfo)) {
 printf("Failed to connect with device\n");
 connection = false;
 }

 char buf[256];
 if (connection) {
 if(readinfo)
 printf("Codename: %s\nVersion: %s\nSerial: %s\nCodebase: %s\nBranch: %s\nLanguage: %s\nRegion: %s\nRomzone: %s\n", codename, version, serial_num, codebase, branch, lang, region, romzone);
        
 if(generate_sign){
 printf("%s", signfile);
 if(!fileexists(signfile)) {
 printf("Please provide firmware file to continue\n");
 } else {
 generate_firmware_sign(signfile);
 }
 }
        
 if(sideloadfile != NULL) {
 if (!fileexists("validate.key")) {
 printf("Sign file not found, please generate it first\n");
 } else {
 if(!fileexists(sideloadfile)){
 printf("Please provide OTA firmware file\n");
 } else {
 start_sideload(sideloadfile);
 }
 }
 }

 if(format_data) {
 printf("Formatting device\n");
 send_recovery_commands("format-data:", buf);
 printf("Device formatted successfully\n");
 }

 send_recovery_commands("reboot:", buf);
 }

 free(codename);
 free(version);
 free(serial_num);
 free(codebase);
 free(branch);
 free(lang);
 free(region);
 free(romzone);
    
 if (dev_handle != NULL) {
 libusb_release_interface(dev_handle, interface_num);
 libusb_close(dev_handle);
 }
 libusb_exit(ctx);

 return 0;
}}}
