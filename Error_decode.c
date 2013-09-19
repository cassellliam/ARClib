#include <Error.h>
#include "ARCbus_internal.h"

const char* cmdtostr(unsigned char cmd){
  switch(cmd){
    case CMD_NACK:
      return "Nack";
    case CMD_SPI_COMPLETE:
      return "SPI Complete";
    case CMD_SPI_RDY:
      return "SPI Ready";
    case CMD_SUB_ON:
      return "Subsystem On";
    case CMD_SUB_OFF:
      return "Subsystem Off";
    case CMD_SUB_POWERUP:
      return "Subsystem Powerup";
    case CMD_RESET:
      return "Reset";
    case CMD_SUB_STAT:
      return "Subsystem Stat";
    case CMD_SPI_CLEAR:
      return "SPI Clear";
    case CMD_EPS_STAT:
      return "EPS Status";
    case CMD_LEDL_STAT:
      return "LEDL Status";
    case CMD_ACDS_STAT:
      return "ACDS Status";
    case CMD_COMM_STAT:
      return "COMM Status";
    case CMD_IMG_STAT:
      return "IMG Status";
    case CMD_ASYNC_SETUP:
      return "Async Setup";
    case CMD_ASYNC_DAT:
      return "Async Data";
    default:
      return "Unknown";
  }
}

//decode errors from ACDS system
char *err_decode_arcbus(char buf[150], unsigned short source,int err, unsigned short argument){
  switch(source){
    case BUS_ERR_SRC_CTL:
      switch(err){
         case CTL_ERR_HANDLER:
           switch(argument){
             case CTL_ERROR_NO_TASKS_TO_RUN: 
              return "CTL : Error Handler : No Tasks To Run";
            case CTL_UNSUPPORTED_CALL_FROM_ISR: 
              return "CTL : Error Handler : Unsupported Call From ISR";
            case CTL_UNSPECIFIED_ERROR:
              return "CTL : Error Handler : Unspecified Error";
            case CTL_MUTEX_UNLOCK_CALL_ERROR:
              return "CTL : Error Handler : Mutex Unlock Call Error";
            default:
              sprintf(buf,"CTL : Error Handler : unknown error = %i",argument);
            return buf;
          }
         break;
      }
    break;
    case BUS_ERR_SRC_MAIN_LOOP:
      switch(err){
        case MAIN_LOOP_ERR_RESET:
          return "ARCbus Main Loop : Commanded Reset";
        case MAIN_LOOP_ERR_CMD_CRC:
          sprintf(buf,"ARCbus Main Loop : bad CRC for command %s (%i)",cmdtostr(argument),argument);
        return buf;
        case MAIN_LOOP_ERR_BAD_CMD:
          sprintf(buf,"ARCbus Main Loop : Bad Command for command %s (%i), resp %i",cmdtostr(argument&0xFF),argument&0xFF,argument>>8);
        return buf;
        case MAIN_LOOP_ERR_NACK_REC:
          sprintf(buf,"ARCbus Main Loop : NACK for command %s (%i), reason %i",cmdtostr(argument>>8),argument>>8,argument&0xFF);
        return buf;
        case MAIN_LOOP_ERR_SPI_COMPLETE_FAIL:
          sprintf(buf,"ARCbus Main Loop : Failed to send SPI complete command : %s",BUS_error_str(argument));
        return buf;
        case MAIN_LOOP_ERR_SPI_CLEAR_FAIL:
          sprintf(buf,"ARCbus Main Loop : Failed to send SPI clear command : %s",BUS_error_str(argument));
        return buf;
        case MAIN_LOOP_ERR_MUTIPLE_CDH:
          return "ARCbus Main Loop : Mutiple CDH boards detected!";
        case MAIN_LOOP_ERR_CDH_NOT_FOUND:
          sprintf(buf,"ARCbus Main Loop : CDH board not found : %s",BUS_error_str(argument));
        return buf;
        case MAIN_LOOP_ERR_RX_BUF_STAT:
          sprintf(buf,"ARCbus Main Loop : Incorrect I2C RX buffer status : %i",argument);
          return buf;
        case MAIN_LOOP_ERR_I2C_RX_BUSY:
          return "ARCbus Main Loop : Rx Buffer busy, Packet Discarded";
        case MAIN_LOOP_ERR_I2C_ARB_LOST:
          return "ARCbus Main Loop : Arbitration Lost";
      }
    break; 
    case BUS_ERR_SRC_STARTUP:
      switch(err){
        case STARTUP_ERR_MAIN_RETURN:
          return "Startup Code : Main Returned";
        case STARTUP_ERR_WDT_RESET:
          return "Startup Code : Watch Dog reset";
        case STARTUP_ERR_POR:
          return "Startup Code : Power On Reset";
        case STARTUP_ERR_RESET_PIN:
          return "Startup Code : Reset Pin Reset";
        case STARTUP_ERR_RESET_FLASH_KEYV:
          return "Startup Code : Flash Security Key Violation";
        case STARTUP_ERR_RESET_UNKNOWN:
          return "Startup Code : Unknown Reset Cause";
        case STARTUP_ERR_RESET_SVS:
          return "Startup Code : Supply Voltage Supervisor Reset";
      }
    break; 
    case BUS_ERR_SRC_ASYNC:
      switch(err){
        case ASYNC_ERR_CLOSE_WRONG_ADDR:
          sprintf(buf,"Async : Wrong closing address. recived 0x%02X expected 0x%02X",argument>>8,argument&0xFF);
          return buf;
        case ASYNC_ERR_OPEN_ADDR:
          sprintf(buf,"Async : can't open addr 0x%02X",argument);
          return buf;
        case ASYNC_ERR_OPEN_BUSY:
          sprintf(buf,"Async : can't open async from addr 0x%02X already connected to 0x%02X",argument>>8,argument&0xFF);
          return buf;
        case ASYNC_ERR_CLOSE_FAIL:
          sprintf(buf,"Async : Failed to send closing command : %s",BUS_error_str(argument));
        return buf;
        case ASYNC_ERR_DATA_FAIL:
          sprintf(buf,"Async : Failed to send data : %s",BUS_error_str(argument));
        return buf;
        case ASYNC_ERR_TX_FLOWCTL:
          switch(argument){
            case ASYNC_FLOW_OFF:
              return "Async : Tx Flow Control : off";
            case ASYNC_FLOW_RUNNING:
              return "Async : Tx Flow Control : running";
            case ASYNC_FLOW_RESTARTING:
              return "Async : Tx Flow Control : restarting";
            case ASYNC_FLOW_STOPPED:  
              return "Async : Tx Flow Control : stopped";
          }
        break;
        case ASYNC_ERR_RX_FLOWCTL:
          switch(argument){
            case ASYNC_FLOW_OFF:
              return "Async : Rx Flow Control : off";
            case ASYNC_FLOW_RUNNING:
              return "Async : Rx Flow Control : running";
            case ASYNC_FLOW_RESTARTING:
              return "Async : Rx Flow Control : restarting";
            case ASYNC_FLOW_STOPPED:  
              return "Async : Rx Flow Control : stopped";
          }
        break;
      }
    break; 
    case BUS_ERR_SRC_SETUP:     
      switch(err){
        case SETUP_ERR_DCO_MISSING_CAL:
          return "ARClib Setup : Missing DCO Calibration Data";
      }
    break;
  }
  sprintf(buf,"source = %i, error = %i, argument = %i",source,err,argument);
  return buf;
}
