#include <stdio.h>
#include <string.h>
#include "iostream"
#include <sstream>

#include <unistd.h>

#include <assert.h>
#include <signal.h>
#include <stdlib.h>

#include <msgpack.hpp>

#include <chrono>
#include <thread>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <CppLinuxSerial/SerialPort.hpp>

#include "rp.h"
#include <sw/redis++/redis++.h>

#include "ringbuffer.hpp"

#include <thread>

#define TCP_PORT 1001
#define PROFILE_BUFFER_SIZE 1024

using namespace std::chrono;

using namespace std;
using namespace sw::redis;
using namespace mn::CppLinuxSerial;

const string STARTUP_TIMESTAMP_KEY ="rover_startup_timestamp";

int64_t startupTimestamp;

int startFrequency   = 1000;
int stepFrequency    = 20;
uint16_t frequencyCount   = 101;
int intermediateFreq = 32;
int transmitPower    = 0;
int loPower          = 15;
uint32_t sampleCount = 101;

static volatile int keepRunning = 1;

SerialPort* rfSource;
Redis* redis;

enum ProfileType : char {
  DUT,
  REF
};

struct RadarProfile {
  uint32_t timestamp;
  
  ProfileType type;

  uint16_t data[];
};

void setFrequency(int frequency, int intermediateFrequency) {
  rfSource->Write("C0");
  rfSource->Write("f" + std::to_string(frequency));
  rfSource->Write("C1\n");
  rfSource->Write("f" + std::to_string(frequency + intermediateFrequency));
}

void enableExcitation(int transmitPower, int loPower) {
  rfSource->Write("C0");
  rfSource->Write("W" + std::to_string(transmitPower));
  rfSource->Write("C1");
  rfSource->Write("W" + std::to_string(loPower));

  rfSource->Write("C0");

  rfSource->Write("E1");
  rfSource->Write("r1");

  rfSource->Write("C1");

  rfSource->Write("E1");
  rfSource->Write("r1");
}

void disableExcitation() {
  rfSource->Write("C0");

  rfSource->Write("E0");
  rfSource->Write("r0");

  rfSource->Write("C1");

  rfSource->Write("E0");
  rfSource->Write("r0");
}

void intHandler(int dummy) {
	if (keepRunning == 0) {
		printf("shutting down!\n");
    disableExcitation();
    
    rp_Release();

    exit(-1);
	}
	keepRunning = 0;
}

void setupSweep(
    int startFrequency, 
    int stepFrequency, 
    uint16_t frequencyCount, 
    int intermediateFrequency,
    float stepTimeInMs) {

  rfSource->Write("w2");

  rfSource->Write("l" + std::to_string(startFrequency));

  rfSource->Write("u" + std::to_string(startFrequency + (stepFrequency * frequencyCount)));

  rfSource->Write("t" + std::to_string(stepTimeInMs));

  rfSource->Write("k" + std::to_string(intermediateFrequency));

  rfSource->Write("n2");

  rfSource->Write("X0");

  rfSource->Write("^1");

  rfSource->Write("c0");
}

void runContinuousSweep() {
  rfSource->Write("c1");
  rfSource->Write("g1");
}

void runSingleSweep() {
  rfSource->Write("g1");
}

jnk0le::Ringbuffer<RadarProfile*, PROFILE_BUFFER_SIZE * 2> profileBuffer;

struct RadarProfile* dutProfileBuffer[PROFILE_BUFFER_SIZE];
struct RadarProfile* refProfileBuffer[PROFILE_BUFFER_SIZE];

void tcpDataServerTask() {
  cpu_set_t mask;

  struct sched_param param;

  memset(&param, 0, sizeof(param));
  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  sched_setscheduler(0, SCHED_FIFO, &param);
  
  CPU_ZERO(&mask);
  CPU_SET(0, &mask);
  sched_setaffinity(0, sizeof(cpu_set_t), &mask);

  int sock_server, sock_client;
  int yes = 1;

  struct sockaddr_in addr; 

  printf("Started tcp server task\n");

  if((sock_server = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    printf("Error opening listening socket\n"); 
    keepRunning = false;
  }

  setsockopt(sock_server, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(TCP_PORT);
  
  if(bind(sock_server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    printf("Error binding to socket\n");
    keepRunning = false;
  }

  listen(sock_server, 1024);
  
  while(keepRunning) {
    if((sock_client = accept(sock_server, NULL, NULL)) < 0) {
      printf("Error accepting connection on socket\n");
      keepRunning = false;
    }

    size_t len = sizeof(struct RadarProfile) + (sampleCount * frequencyCount * sizeof(uint16_t));

    printf("size in bytes %zu\n", len);

    char *data;
    data = (char *) malloc(len);

    while(keepRunning) {
      
      struct RadarProfile* profile = nullptr;

      while(!profileBuffer.remove(profile)) sched_yield();
      
      memcpy(data, profile, len);
      
      size_t offset = 0;
      ssize_t result;
      while (offset < len) {
        result = send(sock_client, data + offset, len - offset, 0);
        if (result < 0) {
          printf("Error sending!\n");
          keepRunning = 0;
        }

        offset += result;
      }
    }
  }
}

int main (int argc, char **argv) {
  signal(SIGABRT, intHandler);
  signal(SIGTERM, intHandler);
  signal(SIGINT, intHandler);

  ConnectionOptions redisConnectionOpts;

  redisConnectionOpts.host = "rover";
  redisConnectionOpts.port = 6379;
  redisConnectionOpts.socket_timeout = std::chrono::milliseconds(5); 
  
  redis = new Redis(redisConnectionOpts);

  auto timestamp = redis->get(STARTUP_TIMESTAMP_KEY);
  if(timestamp) {
    string timestampString = *timestamp;

    startupTimestamp = atoll(timestampString.c_str());
  } else {
    std::cout << "Rover startup timestamp not set or invalid, check key: " << STARTUP_TIMESTAMP_KEY << std::endl;

    exit(0);
  }

  std::cout << "Rover startup timestamp: " << startupTimestamp << std::endl;

  thread tcpDataServerThread(tcpDataServerTask);
  
  cpu_set_t mask;

  struct sched_param param;

  memset(&param, 0, sizeof(param));
  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  sched_setscheduler(0, SCHED_FIFO, &param);
  
  CPU_ZERO(&mask);
  CPU_SET(1, &mask);
  sched_setaffinity(0, sizeof(cpu_set_t), &mask);

  if (rp_Init() != RP_OK) {
    fprintf(stderr, "Red Pitaya API init failed!\n");
    exit(0);
  }

  rp_DpinReset();
  rp_AcqReset();

  rp_acq_decimation_t adc_precision = RP_DEC_1;
  rp_acq_trig_src_t trigger_src = RP_TRIG_SRC_NOW;
  rp_acq_sampling_rate_t sampling_rate = RP_SMP_122_880M; 

  rp_AcqSetSamplingRate(sampling_rate);
  rp_AcqSetDecimation(adc_precision);

  rp_dpin_t stepPin = RP_DIO5_N;
  rp_pinDirection_t direction = RP_OUT;

  rp_DpinSetDirection(stepPin, direction);
  rp_DpinSetState(stepPin, RP_LOW);

  long long int sampleTimeInNs = (1 / ADC_SAMPLE_RATE) * sampleCount * 1000000000;

  rfSource = new SerialPort("/dev/ttyACM0", BaudRate::B_57600, NumDataBits::EIGHT, Parity::NONE, NumStopBits::ONE);
  rfSource->SetTimeout(0);
  rfSource->Open();

  setFrequency(startFrequency, intermediateFreq);
  enableExcitation(transmitPower, loPower);

  setupSweep(startFrequency, stepFrequency, frequencyCount, intermediateFreq, float(sampleTimeInNs / 1000000));

  for(int i = 0; i < PROFILE_BUFFER_SIZE; i++) {
    struct RadarProfile* dutProfile = (RadarProfile *)calloc(1, sizeof(struct RadarProfile) + (sampleCount * frequencyCount * sizeof(uint16_t)));
    struct RadarProfile* refProfile = (RadarProfile *)calloc(1, sizeof(struct RadarProfile) + (sampleCount * frequencyCount * sizeof(uint16_t)));
  
    dutProfile->type = DUT;
    refProfile->type = REF;
 
    dutProfileBuffer[i] = dutProfile;
    refProfileBuffer[i] = refProfile;  
  }

  int currentBufferIndex = 0;

  while(keepRunning) {
    int64_t currentMicro = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();

    if(currentBufferIndex > (PROFILE_BUFFER_SIZE - 1)) currentBufferIndex = 0; 
    
    dutProfileBuffer[currentBufferIndex]->timestamp = uint32_t(currentMicro - startupTimestamp);
    refProfileBuffer[currentBufferIndex]->timestamp = uint32_t(currentMicro - startupTimestamp);

    for(uint16_t i = 0; i < frequencyCount; i++) {
      bool fillState = false;
      rp_AcqStart();

      std::this_thread::sleep_for(std::chrono::nanoseconds(sampleTimeInNs));
    
      rp_AcqSetTriggerSrc(trigger_src);
      rp_acq_trig_state_t state = RP_TRIG_STATE_WAITING;

      while(true) {
        rp_AcqGetTriggerState(&state);
        
        if(state == RP_TRIG_STATE_TRIGGERED) {
          break;
        }
      }

      while(!fillState) {
        rp_AcqGetBufferFillState(&fillState);
      }

      rp_AcqStop();
      
      rp_AcqGetDataRawV2(0, 
        &sampleCount, 
        &refProfileBuffer[currentBufferIndex]->data[i * sampleCount * sizeof(uint16_t)],
        &dutProfileBuffer[currentBufferIndex]->data[i * sampleCount * sizeof(uint16_t)]);

      profileBuffer.insert(&dutProfileBuffer[currentBufferIndex]);
      profileBuffer.insert(&refProfileBuffer[currentBufferIndex]);

      //rp_AcqGetDataV2(0, &sampleCount, &dut_buff[i], &ref_buff[i]);

      rp_DpinSetState(stepPin, RP_HIGH);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      rp_DpinSetState(stepPin, RP_LOW);
    }
 
    currentBufferIndex++; 
    int64_t endTime = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    
    printf("Sweep Done, took %lld microseconds\n", endTime - currentMicro);
  }

  for(int i = 0; i < PROFILE_BUFFER_SIZE; i++) {
    free(dutProfileBuffer[i]);
    free(refProfileBuffer[i]);
  }

  disableExcitation();
  
  rp_Release();

  return 0;
}
