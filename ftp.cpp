// Store SD card content on a remote server using FTP
//
// s60sc 2022, based on code contributed by gemi254

#include "myConfig.h"

// Ftp server params
char ftp_server[32] = "";
char ftp_port[6]    = "";
char ftp_user[32]   = "";
char ftp_pass[MAX_PWD_LEN] = "";
char ftp_wd[64]     = "";

#define RESPONSE_TIMEOUT 10000  

// FTP control
static char rspBuf[256]; // Ftp response buffer
static char respCodeRx[4]; // ftp response code                        
static TaskHandle_t FTPtaskHandle = NULL;
static char sdPathName[FILE_NAME_LEN];
static bool uploadInProgress = false;

// WiFi Clients
WiFiClient client;
WiFiClient dclient;

static bool sendFtpCommand(const char* cmd, const char* param, const char* respCode) {
  // build and send ftp command
  if (strlen(cmd)) {
    client.print(cmd);
    client.println(param);
  }
  LOG_DBG("Sent cmd: %s%s", cmd, param);
  
  // wait for ftp server response
  uint32_t start = millis();
  while (!client.available() && millis() < start + RESPONSE_TIMEOUT) delay(1);
  if (!client.available()) {
    LOG_ERR("Timed out waiting for client response");
    return false;
  }
  // read in response code and message
  client.read((uint8_t*)respCodeRx, 3); 
  respCodeRx[3] = 0; // terminator
  int readLen = client.read((uint8_t*)rspBuf, 255);
  rspBuf[readLen] = 0;
  while (client.available()) client.read(); // bin the rest of response

  // check response code with expected
  LOG_DBG("Rx code: %s, resp: %s", respCodeRx, rspBuf);
  if (strcmp(respCode, "999") == 0) return true; // response code not checked
  if (strcmp(respCodeRx, respCode) != 0) {
    // response code wrong
    LOG_ERR("Command %s got wrong response: %s", cmd, rspBuf);
    return false;
  }
  return true;
}

static bool ftpConnect(){
  // Connect to ftp and change to root dir
  if (client.connect(ftp_server, atoi(ftp_port))) {
    LOG_DBG("FTP connected at %s:%s", ftp_server, ftp_port);
  } else {
    LOG_ERR("Error opening ftp connection to %s:%s", ftp_server, ftp_port);
    return false;
  }
  if (!sendFtpCommand("", "", "220")) return false;
  if (!sendFtpCommand("USER ", ftp_user, "331")) return false;
  if (!sendFtpCommand("PASS ", ftp_pass, "230")) return false;
  if (!sendFtpCommand("CWD ", ftp_wd, "250")) return false;
  if (!sendFtpCommand("Type I", "", "200")) return false;
  return true;
}

static bool createFtpFolder(const char* folderName) {
  // create folder if non existent then change to it
  LOG_DBG("Check for folder %s", folderName);
  sendFtpCommand("CWD ", folderName, "999"); 
  if (strcmp(respCodeRx, "550") == 0) {
    // non existent folder, create it
    if (!sendFtpCommand("MKD ", folderName, "257")) return false;
    if (!sendFtpCommand("CWD ", folderName, "250")) return false;         
  }
  return true;
}

static bool getFolderName(const char* folderPath) {
  // extract folder names from path name
  char folderName[FILE_NAME_LEN];
  strcpy(folderName, folderPath); 
  int pos = 1; // skip 1st '/'
  // get each folder name in sequence
  for (char* p = strchr(folderName, '/'); (p = strchr(++p, '/')) != NULL; pos = p + 1 - folderName) {
    *p = 0; // terminator
    if (!createFtpFolder(folderName + pos)) return false;
  }
  return true;
}

static bool openDataPort() {
  // set up port for data transfer
  if (!sendFtpCommand("PASV", "", "227")) return false;
  // derive data port number
  char* p = strchr(rspBuf, '('); // skip over initial text
  int p1, p2;   
  int items = sscanf(p, "(%*d,%*d,%*d,%*d,%d,%d)", &p1, &p2);
  if (items != 2) {
    LOG_ERR("Failed to parse data port");
    return false;
  }
  int dataPort = (p1 << 8) + p2;
  
  // Connect to data port
  LOG_DBG("Data port: %i", dataPort);
  if (!dclient.connect(ftp_server, dataPort)) {
    LOG_ERR("Data connection failed");   
    return false;
  }
  return true;
}

static bool ftpStoreFile(File &fh) {
  // Upload individual file to current folder, overwrite any existing file  
  if (strstr(fh.name(), FILE_EXT) == NULL) return false; // folder, or not valid file type    
  // determine if file is suitable for conversion to AVI
  char ftpSaveName[FILE_NAME_LEN];
  if (isAVI(fh)) changeExtension(ftpSaveName, fh.name(), "avi");
  else strcpy(ftpSaveName, fh.name());
  size_t fileSize = fh.size();
  LOG_INF("Upload file: %s, size: %0.1fMB", ftpSaveName, (float)(fileSize)/ONEMEG);    

  // open data connection
  openDataPort();
  uint32_t writeBytes = 0, progCnt = 0;                       
  uint32_t uploadStart = millis();
  size_t readLen, writeLen;
  if (!sendFtpCommand("STOR ", ftpSaveName, "150")) return false; 
  do {
    // upload file in chunks
    readLen = readClientBuf(fh, chunk, RAMSIZE); // obtain modified data to send 
    if (readLen) {
      writeLen = dclient.write((const uint8_t*)chunk, readLen);
      writeBytes += writeLen;
      if (writeLen == 0) {
        LOG_ERR("Upload file to ftp failed");
        return false;
      }
      progCnt++;
      if (progCnt % 50 == 0) LOG_INF("Uploaded %u%%", writeBytes * 100 / fileSize);      
    }
  } while (readLen > 0);
  dclient.stop();
  if (sendFtpCommand("", "", "226")) LOG_INF("Uploaded %0.1fMB in %u sec", (float)(writeBytes) / ONEMEG, (millis() - uploadStart) / 1000); 
  else LOG_ERR("File transfer not successful");
  return true;
}

static void uploadFolderOrFileFtp() {
  // Upload a single file or whole folder using ftp 
  // folder is uploaded file by file
  if (strlen(sdPathName) < 2){
    LOG_DBG("Root or null is not allowed %s", sdPathName);  
    return;  
  }
  if (!ftpConnect()) {
    LOG_ERR("Unable to make ftp connection");
    return; 
  }
  File root = SD_MMC.open(sdPathName);
  if (!root) {
    LOG_ERR("Failed to open: %s", sdPathName);
    return;
  }  
   
  if (!root.isDirectory()) {
    // Upload a single file 
    if (getFolderName(root.path())) ftpStoreFile(root); 
  } else {  
    // Upload a whole folder, file by file
    LOG_INF("Uploading folder: ", root.name()); 
    if (!createFtpFolder(root.name())) return;
    File fh = root.openNextFile();            
    while (fh) {
      if (!ftpStoreFile(fh)) break; // abandon rest of files
      fh.close();
      fh = root.openNextFile();
    }
    if (fh) fh.close();
  }
  root.close();
}

static void FTPtask(void* parameter) {
  // wait for wake up to process an FTP request
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    doPlayback = false; // close any current playback
    uploadFolderOrFileFtp();
    // Disconnect from ftp server
    client.println("QUIT");
    dclient.stop();
    client.stop();
    uploadInProgress = false;
  }
  vTaskDelete(NULL);
}

void startFTPtask() {
  // create task on startup
  xTaskCreate(&FTPtask, "FTPtask", 4096*2, NULL, 1, &FTPtaskHandle);    
}

bool ftpFileOrFolder(const char* fileFolder) {
  // called from other functions to commence FTP upload
  if (!uploadInProgress) {
    uploadInProgress = true;
    strcpy(sdPathName, fileFolder);
    xTaskNotifyGive(FTPtaskHandle);
    return true;
  } else LOG_ERR("Unable to upload %s as another upload in progress", fileFolder);
  return false;
}
