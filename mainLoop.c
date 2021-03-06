#include <ctl.h>
#include <msp430.h>
#include <stdlib.h>
#include <string.h>
#include "timerA.h"
#include "ARCbus.h"
#include "crc.h"
#include "spi.h"
#include <Error.h>
#include "ARCbus_internal.h"

//record error function, used to save an error without it cluttering up the terminal
void record_error(unsigned char level,unsigned short source,int err, unsigned short argument,ticker time);

//bus internal events
CTL_EVENT_SET_t BUS_INT_events,BUS_helper_events;

//task structure for idle task and ARC bus task
CTL_TASK_t idle_task,ARC_bus_task,ARC_bus_helper_task;

//stack for ARC bus task
unsigned BUS_stack[256],helper_stack[250];

BUS_STAT arcBus_stat;

//events for subsystems
CTL_EVENT_SET_t SUB_events;

//address of SPI slave during transaction
static unsigned char SPI_addr=0;

static void ARC_bus_helper(void *p);

static struct{
    CTL_MUTEX_t mutex;
    unsigned short size;
    unsigned char type;
    unsigned char level;
    unsigned char dest;
}err_req;

//struct for NACK info
//This is used to setup a NACK packet in the bus task and have it sent by the bus helper task
//no mutex is used but address is used to indicate busy status. This only works if no other threads use this structure
static struct{
  unsigned char addr;
  unsigned char dat[BUS_I2C_HDR_LEN+2+BUS_I2C_CRC_LEN];
}nack_info;


//power state of subsystem
unsigned short powerState=SUB_PWR_OFF;

//pointer to linked list of command parse functions
//nodes in the list are sorted by priority
//this node is the first in the list (or NULL for an empty list)
//the last node in the list has next set to NULL
CMD_PARSE_DAT *cmd_parse_list=NULL;

//register callback
//insert the callback structure in the linked list based on priority
void BUS_register_cmd_callback(CMD_PARSE_DAT *cb_dat){
  //get a pointer to the current list head
  CMD_PARSE_DAT **head=&cmd_parse_list;
  //find insertion point.
  //This should be where *head points to a node with greater priority (or the beginning)
  //and *head->next points to a node with lower priority (or the end)
  while(*head!=NULL && (*head)->priority>cb_dat->priority){
    //move head to the next element in the list
    head= &(*head)->next;
  }
  //link to lower priority callbacks
  cb_dat->next=*head;
  //link in this callback
  *head=cb_dat;
}
#define BUS_VERSION_LEN         (sizeof(BUS_VERSION)+BUS_VERSION_HASH_LEN)
#define BUS_VERSION_MINOR_DIG   (4)     //maximum digits in minor version
#define BUS_VERSION_HASH_LEN    (13)    //maximum length of hash that is sent


//compare versions and report error if they are different
char BUS_version_cmp(const BUS_VERSION* other,unsigned char len){
  //first check that length is long enough
  if(len<sizeof(BUS_VERSION)){
    //return error
    return BUS_VER_LENGTH;
  }
  //check for valid major versions
  if(other->major==BUS_INVALID_MAJOR_VER || ARClib_vstruct.major==BUS_INVALID_MAJOR_VER){
    //report error
    report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_INVALID_MAJOR,(ARClib_vstruct.major==BUS_INVALID_MAJOR_VER?VERSION_ERR_INVALID_MINE:0)|(other->major==BUS_INVALID_MAJOR_VER?VERSION_ERR_INVALID_OTHER:0));
    //return, a major version is invalid, can't continue
    return BUS_VER_INVALID_MAJOR_REV;
  }
  //compare major versions
  if(other->major!=ARClib_vstruct.major){
    //versions diffe, check which one is older
    if(other->major>ARClib_vstruct.major){
      //report error
      report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_MAJOR_REV_NEWER,other->major);
      //other version is newer
      return BUS_VER_MAJOR_REV_NEWER;
    }else{
      //report error
      report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_MAJOR_REV_OLDER,other->major);
      //other version is older
      return BUS_VER_MAJOR_REV_NEWER;
    }
  }
  //check for valid minor versions
  if(other->minor==BUS_INVALID_MINOR_VER || ARClib_vstruct.minor==BUS_INVALID_MINOR_VER){
    //report error
    report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_INVALID_MINOR,(ARClib_vstruct.minor==BUS_INVALID_MINOR_VER?VERSION_ERR_INVALID_MINE:0)|(other->minor==BUS_INVALID_MINOR_VER?VERSION_ERR_INVALID_OTHER:0));
    //return, a major version is invalid, can't continue
    return BUS_VER_INVALID_MINOR_REV;
  }
  //compare minor versions
  if(other->minor!=ARClib_vstruct.minor){
    //versions diffe, check which one is older
    if(other->minor>ARClib_vstruct.minor){
      //report error
      report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_MINOR_REV_NEWER,other->minor);
      //other version is newer
      return BUS_VER_MINOR_REV_NEWER;
    }else{
      //report error
      report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_MINOR_REV_OLDER,other->minor);
      //other version is older
      return BUS_VER_MINOR_REV_OLDER;
    }
  }
  //check dirty flags
  if(other->dty!=BUS_VER_CLEAN || ARClib_vstruct.dty!=BUS_VER_CLEAN){
    //report error
    report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_DIRTY_REV,(((unsigned short)ARClib_vstruct.dty)<<8)|(other->dty));
    //one version is dirty, there is no way to tell if they are the same
    return BUS_VER_DIRTY_REV;
  }
  //compare hashes
  if(strncmp(other->hash,ARClib_vstruct.hash,len-sizeof(BUS_VERSION))){
    //report error
    report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_HASH_MISMATCH,0);
    //hashes are different
    return BUS_VER_HASH_MISMATCH;
  }
  //check string length of hash to make sure the full hash was compared
  if(strlen(ARClib_vstruct.hash)+sizeof(BUS_VERSION)>len){
    //compare the number of commits between versioned commit and the current one
    if(other->commits!=ARClib_vstruct.commits){
      //report error
      report_error(ERR_LEV_ERROR,BUS_ERR_SRC_VERSION,VERSION_ERR_COMMIT_MISMATCH,other->commits);
      //different number of commits, different commits
      return BUS_VER_COMMIT_MISMATCH;
    }
    //otherwise versions are probably the same
  }
  return BUS_VER_SAME;
}

//ARC bus Task, do ARC bus stuff
static void ARC_bus_run(void *p) __toplevel{
  unsigned int e;
  unsigned char len;
  unsigned char addr,cmd,flags;
  int resp;
  unsigned char pk[40];
  unsigned char *ptr;
  unsigned short crc;
  unsigned char *SPI_buf=NULL;
  ticker nt,ot;
  int snd,i;
  unsigned char parse_mask;
  #ifdef CDH_LIB
  //temporary array for bus version comparison, needed for alignment reasons
  unsigned short tmp[(BUS_VERSION_LEN+1)/sizeof(unsigned short)];
  #endif
  CMD_PARSE_DAT *parse_ptr;
  SPI_addr=0;
  //Initialize ErrorLib
  error_recording_start();
  //init error request mutex
  ctl_mutex_init(&err_req.mutex);
  //initialize helper events
  ctl_events_init(&BUS_helper_events,0);
  //start helper task
  ctl_task_run(&ARC_bus_helper_task,BUS_PRI_ARCBUS_HELPER,ARC_bus_helper,NULL,"ARC_Bus_helper",sizeof(helper_stack)/sizeof(helper_stack[0])-2,helper_stack+1,0);
  
  //event loop
  for(;;){
    //wait for something to happen
    e = ctl_events_wait(CTL_EVENT_WAIT_ANY_EVENTS_WITH_AUTO_CLEAR,&BUS_INT_events,BUS_INT_EV_ALL,CTL_TIMEOUT_NONE,0);
    //check if buffer can be unlocked
    if(e&BUS_INT_EV_BUFF_UNLOCK){
      SPI_buf=NULL;
      //unlock buffer
      BUS_free_buffer();
    }
    //check if I2C mutex can be released
    if(e&BUS_INT_EV_RELEASE_MUTEX){
      //release I2C mutex
      BUS_I2C_release();
    }
    //check if a SPI transaction is complete
    if(e&BUS_INT_EV_SPI_COMPLETE){
      //check if SPI was in progress
      if(SPI_addr){
        //turn off SPI
        SPI_deactivate();
        //assemble CRC
        crc=SPI_buf[arcBus_stat.spi_stat.len+1];//LSB
        crc|=(((unsigned short)SPI_buf[arcBus_stat.spi_stat.len])<<8);//MSB
        //check CRC
        if(crc!=crc16(SPI_buf,arcBus_stat.spi_stat.len)){
          //Bad CRC
          //clear buffer pointer
          SPI_buf=NULL;
          //free buffer
          BUS_free_buffer();
          //send event
          ctl_events_set_clear(&SUB_events,SUB_EV_SPI_ERR_CRC,0);
          //set return value for SPI complete packet
          arcBus_stat.spi_stat.nack=ERR_BAD_CRC;
        }else{
          //tell subsystem, SPI data received
          //Subsystem must signal to free the buffer
          ctl_events_set_clear(&SUB_events,SUB_EV_SPI_DAT,0);
          //set return value for SPI complete packet
          arcBus_stat.spi_stat.nack=RET_SUCCESS;
        }
        //tell helper thread to send SPI complete command
        ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_SPI_COMPLETE_CMD,0);
      }
    }
    //check if an I2C command has been received
    if(e&BUS_INT_EV_I2C_CMD_RX){
        //check if packet is complete
        if(I2C_rx_buf[I2C_rx_out].stat!=I2C_PACKET_STAT_COMPLETE){
            //report error
            report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_RX_BUF_STAT,I2C_rx_buf[I2C_rx_out].stat);
            //disable interrupts
            ctl_global_interrupts_set(0);
            //put UCB0 into reset state
            UCB0CTL1|=UCSWRST;   
            //initialize I2C packet queue to empty state
            for(i=0;i<BUS_I2C_PACKET_QUEUE_LEN;i++){
                I2C_rx_buf[i].stat=I2C_PACKET_STAT_EMPTY;
            }
            //I2C mutex init
            ctl_mutex_init(&arcBus_stat.i2c_stat.mutex);
            //set I2C to idle mode
            arcBus_stat.i2c_stat.mode=BUS_I2C_IDLE;
            //initialize I2C packet queue pointers
            I2C_rx_in=I2C_rx_out=0;
            //bring UCB0 out of reset state
            UCB0CTL1&=~UCSWRST;
            //re-enable interrupts
            ctl_global_interrupts_enable();
        }else{
        //clear response
        resp=0;
        //get len
        len=I2C_rx_buf[I2C_rx_out].len;
        //compute crc
        crc=crc7(I2C_rx_buf[I2C_rx_out].dat,len-1);
        //get length of payload
        len=len-BUS_I2C_CRC_LEN-BUS_I2C_HDR_LEN;
        //get sender address
        addr=CMD_ADDR_MASK&I2C_rx_buf[I2C_rx_out].dat[0];
        //get packet flags
        flags=I2C_rx_buf[I2C_rx_out].flags;
        //get command type
        cmd=I2C_rx_buf[I2C_rx_out].dat[1];
        //point to the first payload byte
        ptr=&I2C_rx_buf[I2C_rx_out].dat[2];
        //check crc for packet
        if(ptr[len]==crc){
          //handle command based on command type
          switch(cmd){
            case CMD_SUB_ON:            
                //check for proper length
                if(len!=0){
                  resp=ERR_PK_LEN;
                }
                //set new power status
                powerState=SUB_PWR_ON;
                //inform subsystem
                ctl_events_set_clear(&SUB_events,SUB_EV_PWR_ON,0);
            break;
            case CMD_SUB_OFF:
              //check to make sure that the command is directed to this subsystem
              if(len==1 && BUS_OA_check(ptr[0])==RET_SUCCESS){
                //set new power status
                powerState=SUB_PWR_OFF;
                //inform subsystem
                ctl_events_set_clear(&SUB_events,SUB_EV_PWR_OFF,0);
              }else{
                //error with command
                resp=ERR_BAD_PK;
              }
            break;
            case CMD_SUB_STAT:
              //check for proper length
              if(len!=4){
                resp=ERR_PK_LEN;
              }                
              #ifndef CDH_LIB //only update time on subsystem boards
                  //assemble time from packet
                  nt=ptr[3];
                  nt|=((ticker)ptr[2])<<8;
                  nt|=((ticker)ptr[1])<<16;
                  nt|=((ticker)ptr[0])<<24;
                  //update time
                  ot=setget_ticker_time(nt);
                  //tell subsystem to send status
                  ctl_events_set_clear(&SUB_events,SUB_EV_SEND_STAT,0);
                  //trigger any alarms that were skipped
                  BUS_alarm_ticker_update(nt,ot);
              #else
                  //if CMD_SUB_STAT is recived by CDH, report an error
                  report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_CDH_SUB_STAT_REC,addr);
                  resp=ERR_ILLEAGLE_COMMAND;
              #endif
            break;
            case CMD_RESET:          
              //check for proper length
              if(len!=0){
                resp=ERR_PK_LEN;
              }
              //reset msp430
              reset(ERR_LEV_INFO,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_RESET,0);
              //code should never get here, report error
              report_error(ERR_LEV_CRITICAL,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_RESET_FAIL,0);
              break;
            case CMD_SPI_RDY:
              //check length
              if(len!=2){
                resp=ERR_PK_LEN;
                break;
              }
              //assemble length
              arcBus_stat.spi_stat.len=ptr[1];//LSB
              arcBus_stat.spi_stat.len|=(((unsigned short)ptr[0])<<8);//MSB
              //check length account for 16bit CRC
              if(arcBus_stat.spi_stat.len+2>BUS_get_buffer_size()){
                //length is too long
                //cause NACK to be sent
                resp=ERR_SPI_LEN;
                break;
              }
              //check if already transmitting
              if(SPI_buf!=NULL){
                resp=ERR_SPI_BUSY;
                break;
              }
              SPI_buf=BUS_get_buffer(CTL_TIMEOUT_NOW,0);
              //check if buffer was locked
              if(SPI_buf==NULL){
                //buffer locked, set event
                ctl_events_set_clear(&SUB_events,SUB_EV_SPI_ERR_BUSY,0);
                //set response
                resp=ERR_BUFFER_BUSY;
                //stop SPI setup
                break;
              }
              //disable DMA
              DMA0CTL&=~DMAEN;
              DMA1CTL&=~DMAEN;
              DMA2CTL&=~DMAEN;
              //save address of SPI slave
              SPI_addr=addr;
              //setup SPI structure
              arcBus_stat.spi_stat.rx=SPI_buf;
              arcBus_stat.spi_stat.tx=NULL;
              //Setup SPI bus to exchange data as master
              SPI_master_setup();
              //============[setup DMA for transfer]============
              //setup source trigger
              DMACTL0 &=~(DMA0TSEL_31|DMA1TSEL_31);
              DMACTL0 |= (DMA0TSEL__USCIA0RX|DMA1TSEL__USCIA0TX);
              DMACTL1 = DMA2TSEL__USCIA0RX;
              //DMA9 workaround, use a dummy channel with lower priority and the same trigger
              //setup dummy channel: read and write from unused space in the SPI registers
              *((unsigned int*)&DMA2SA) = EUSCI_A0_BASE + 0x02;
              *((unsigned int*)&DMA2DA) = EUSCI_A0_BASE + 0x04;
              // only one byte
              DMA2SZ = 1;
              // Configure the DMA transfer, repeated byte transfer with no increment
              DMA2CTL = DMADT_4|DMASBDB|DMAEN|DMASRCINCR_0|DMADSTINCR_0;

              // Source DMA address: receive register.
              *((unsigned int*)&DMA0SA) = (unsigned short)(&UCA0RXBUF);
              // Destination DMA address: rx buffer.
              *((unsigned int*)&DMA0DA) = (unsigned short)SPI_buf;
              // The size of the block to be transferred
              DMA0SZ = arcBus_stat.spi_stat.len+BUS_SPI_CRC_LEN;
              // Configure the DMA transfer, single byte transfer with destination increment
              DMA0CTL = DMAIE|DMADT_0|DMASBDB|DMAEN|DMASRCINCR_0|DMADSTINCR_3;

              // Source DMA address: SPI transmit buffer, constant data will be sent
              *((unsigned int*)&DMA1SA) = (unsigned int)(&UCA0TXBUF);
              // Destination DMA address: the transmit buffer.
              *((unsigned int*)&DMA1DA) = (unsigned int)(&UCA0TXBUF);
              // The size of the block to be transferred
              DMA1SZ = arcBus_stat.spi_stat.len+BUS_SPI_CRC_LEN-1;
              // Configure the DMA transfer, single byte transfer with no increment
              DMA1CTL=DMADT_0|DMASBDB|DMAEN|DMASRCINCR_0|DMADSTINCR_0;
              //write the Tx buffer to start transfer
              UCA0TXBUF=BUS_SPI_DUMMY_DATA;
            break;
            
            case CMD_SPI_ABORT:
              //check length
              if(len!=0){
                resp=ERR_PK_LEN;
                break;
              }
              //check SPI mode
              if(arcBus_stat.spi_stat.mode!=BUS_SPI_MASTER){
                resp=ERR_SPI_NOT_RUNNING;
                break;
              }
              //check SPI address
              if(SPI_addr!=addr){
                resp=ERR_SPI_WRONG_ADDR;
                break;
              }
              //disable DMA
              DMA0CTL&=~DMAEN;
              DMA1CTL&=~DMAEN;
              DMA2CTL&=~DMAEN;
              //turn off SPI
              SPI_deactivate();              
              //clear buffer pointer
              SPI_buf=NULL;
              //free buffer
              BUS_free_buffer();
              //clear address
              SPI_addr=0;
              //retport error
              report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_SPI_ABORT,addr);
            break;

            case CMD_SPI_COMPLETE:
              //check length
              if(len!=1){
                resp=ERR_PK_LEN;
                break;
              }
#ifndef CDH_LIB
              //check if a SPI transaction was in progress
              if(arcBus_stat.spi_stat.mode!=BUS_SPI_SLAVE){
#else
              //check if a SPI transaction was in progress
              if(arcBus_stat.spi_stat.mode!=BUS_SPI_MASTER && arcBus_stat.spi_stat.mode!=BUS_SPI_SLAVE){
#endif
                //SPI is in the wrong state so send busy error
                resp=ERR_SPI_NOT_RUNNING;
                //send NACK
                break;
              }

              //check that the command came from the correct subsystem
              if(arcBus_stat.spi_stat.mode==BUS_SPI_MASTER && SPI_addr!=addr){
                  //wrong address sent for complete command
                  resp=ERR_SPI_WRONG_ADDR;
                  //send NACK
                  break;
              }
              //disable DMA
              DMA0CTL&=~DMAEN;
              DMA1CTL&=~DMAEN;
              DMA2CTL&=~DMAEN;
              //turn off SPI
              SPI_deactivate();
              //SPI transfer is done, see if there was an error
              arcBus_stat.spi_stat.nack=ptr[0];
              //notify CDH board
#ifndef CDH_LIB
              ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_SPI_CLEAR_CMD,0);
#endif
              //notify calling task
              ctl_events_set_clear(&arcBus_stat.events,BUS_EV_SPI_COMPLETE,0);
            break;
            case CMD_ASYNC_SETUP:
              //check length
              if(len!=1){
                resp=ERR_PK_LEN;
                break;
              }
              switch(ptr[0]){
                case ASYNC_OPEN:
                  //open remote connection
                  async_open_remote(addr);
                break;
                case ASYNC_CLOSE:
                  //check if sending address corosponds to async address
                  if(async_addr!=addr){
                    //report error
                    report_error(ERR_LEV_ERROR,BUS_ERR_SRC_ASYNC,ASYNC_ERR_CLOSE_WRONG_ADDR,(((unsigned short)addr)<<8)|async_addr);
                    break;
                  }
                  //tell helper thread to close connection
                  ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_ASYNC_CLOSE,0);
                break;
              }
            break;
            case CMD_ASYNC_DAT:
              //post bytes to queue
              ctl_byte_queue_post_multi_nb(&async_rxQ,len,ptr);
            break;
            case CMD_NACK:
              //TODO: handle this better somehow?
              //check length
              if(len!=2){
                resp=ERR_PK_LEN;
                break;
              }
              //set event 
              ctl_events_set_clear(&arcBus_stat.events,BUS_EV_CMD_NACK,0);
              //report error
              report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_NACK_REC,(((unsigned short)ptr[0])<<8)|((unsigned short)ptr[1]));
              //check which packet was nacked
              switch(ptr[0]){
                  case CMD_SPI_RDY:
                    //set SPI nack reason
                    arcBus_stat.spi_stat.nack=ptr[1];
                    //send event to spi code
                    ctl_events_set_clear(&arcBus_stat.events,BUS_EV_SPI_NACK,0);
                  break;
              }
            break;
            case CMD_ERR_REQ:
              if(len<1){
                resp=ERR_PK_LEN;
                break;
              }
              if(!ctl_mutex_lock(&err_req.mutex,CTL_TIMEOUT_NOW,0)){
                resp=ERR_BUSY;
                break;
              }
              //request type
              err_req.type=ptr[0];
              //address to send data to
              err_req.dest=addr;
              switch(ptr[0]){
                case ERR_REQ_REPLAY:
                    err_req.size=(((unsigned short)ptr[1])<<8)|((unsigned short)ptr[2]);
                    err_req.level=ptr[3];
                break;
                default:
                    resp=ERR_INVALID_ARGUMENT;
                break;
              }
              //check if the packet was parsed
              if(!resp){
                //send event to process request
                ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_ERR_REQ,0);
              }
            ctl_mutex_unlock(&err_req.mutex);
            break;
            case CMD_PING:
                //this is a dummy command that does nothing
            break;
            default:
            #ifdef CDH_LIB
              if(cmd==CMD_SUB_POWERUP){
                char vresp;
                //copy into temporary word aligned variable
                memcpy(tmp,ptr,len);
                //compare to version string
                if((vresp=BUS_version_cmp((BUS_VERSION*)tmp,len))){
                    //version mismatch
                    report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_SUBSYSTEM_VERSION_MISMATCH,(((unsigned short)vresp)<<8)|addr);
                }
                //set length to zero
                len=0;
              }
            #endif
              //get callback structure list
              parse_ptr=cmd_parse_list;
              //get mask from packet flags
              parse_mask=flags;
              //set response to unknown command
              resp=ERR_UNKNOWN_CMD;
              //loop through list and check for commands
              while(parse_ptr!=NULL && resp==ERR_UNKNOWN_CMD){                
                //check if flags match
                if(parse_ptr->flags&parse_mask){
                  //check for subsystem command
                  resp=parse_ptr->cb(addr,cmd,ptr,len,flags);
                }
                //get next callback structure
                parse_ptr=parse_ptr->next;
              }
            break;
          }
          //check if command was recognized
          if(resp!=0){
            report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_BAD_CMD,(((unsigned short)resp)<<8)|((unsigned short)cmd));
            //check packet to see if NACK should be sent
            if(I2C_rx_buf[I2C_rx_out].dat[0]&CMD_TX_NACK){
              //check NACK address to see if a nack can be sent
              if(nack_info.addr==0){
                //set address
                nack_info.addr=addr;
                //setup command
                ptr=BUS_cmd_init(nack_info.dat,CMD_NACK);
                //sent command
                *ptr++=cmd;
                //send NACK reason
                *ptr++=resp;
                //tell helper thread to send packet
                ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_NACK,0);
              }else{
                //can't send nack, report error
                report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_NACK_BUSY,(((unsigned short)addr)<<8)|resp);
              }
            }
          }
        }else{
          //CRC failed, report error
          report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_CMD_CRC,cmd);
          //if command was not a NACK command send NACK
          if(cmd!=CMD_NACK){
            //check NACK address to see if a nack can be sent
            if(nack_info.addr==0){
              //set address
              nack_info.addr=addr;
              //setup command
              ptr=BUS_cmd_init(nack_info.dat,CMD_NACK);
              //sent command
              *ptr++=cmd;
              //send NACK reason
              *ptr++=ERR_BAD_CRC;
              //tell helper thread to send packet
              ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_NACK,0);
            }else{
              //can't send nack, report error
              report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_NACK_BUSY,(((unsigned short)addr)<<8)|ERR_BAD_CRC);
            }
          }
        }
        //done with packet set status
        I2C_rx_buf[I2C_rx_out].stat=I2C_PACKET_STAT_EMPTY;
        //increment index
        I2C_rx_out++;
        //check for wraparound
        if(I2C_rx_out>=BUS_I2C_PACKET_QUEUE_LEN){
          I2C_rx_out=0;
        }
        //check next packet status
        if(I2C_rx_buf[I2C_rx_out].stat==I2C_PACKET_STAT_COMPLETE){   
          //There is still a packet set event again
          ctl_events_set_clear(&BUS_INT_events,BUS_INT_EV_I2C_CMD_RX,0);
        }
      }
    }
    //check for errors and report
    if(e&BUS_INT_EV_I2C_RX_BUSY){
      report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_I2C_RX_BUSY,0);
    }
    if(e&BUS_INT_EV_I2C_ARB_LOST){
      report_error(ERR_LEV_DEBUG,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_I2C_ARB_LOST,0);
    }
    //Low side supply error
    if(e&BUS_INT_EV_SVML){
      //report error
      report_error(ERR_LEV_CRITICAL,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_SVML,0);
    }
    //high side supply error
    if(e&BUS_INT_EV_SVMH){
      //report error
      report_error(ERR_LEV_CRITICAL,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_SVMH,0);
    }
  }
}
    
    
//ARC bus Task, do ARC bus stuff
static void ARC_bus_helper(void *p) __toplevel{
  unsigned int e;
  int resp,maxsize;
  unsigned char *ptr,pk[BUS_I2C_HDR_LEN+BUS_VERSION_LEN+BUS_I2C_CRC_LEN];
  unsigned short len;
  #ifndef CDH_LIB         //Subsystem board 
    //first send "I'm on" command
    ptr=BUS_cmd_init(pk,CMD_SUB_POWERUP);//setup command
    //copy in data part of struct
    memcpy(ptr,&ARClib_vstruct,sizeof(BUS_VERSION));
    //increment pointer
    ptr+=sizeof(BUS_VERSION);
    //write version into string
    len=strlcpy((char*)ptr,ARClib_vstruct.hash,BUS_VERSION_HASH_LEN)+sizeof(BUS_VERSION);
    //send command
    resp=BUS_cmd_tx(BUS_ADDR_CDH,pk,len,0);
      //check for failed send
      if(resp!=RET_SUCCESS){
        //give a warning
        report_error(ERR_LEV_WARNING,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_CDH_NOT_FOUND,resp);
        //wait a bit
        ctl_timeout_wait(ctl_get_current_time()+30);
        //resend
        resp=BUS_cmd_tx(BUS_ADDR_CDH,pk,len,0);
        //check for success
        if(resp!=RET_SUCCESS){
          //Failed
          report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_CDH_NOT_FOUND,resp);     
        }
      }
  #endif
  for(;;){
    e=ctl_events_wait(CTL_EVENT_WAIT_ANY_EVENTS_WITH_AUTO_CLEAR,&BUS_helper_events,BUS_HELPER_EV_ALL,CTL_TIMEOUT_NONE,0);
    //async timer timed out, send data
    if(e&BUS_HELPER_EV_ASYNC_TIMEOUT){
      //send some data
      async_send_data();
    }
    //SPI transaction is complete
    if(e&BUS_HELPER_EV_SPI_COMPLETE_CMD){      
      //done with SPI send command
      ptr=BUS_cmd_init(pk,CMD_SPI_COMPLETE);
      //send return to indicate success
      *ptr=arcBus_stat.spi_stat.nack;
      //send data
      resp=BUS_cmd_tx(SPI_addr,pk,1,0);
      //check if command was successful and try again if it failed
      if(resp!=RET_SUCCESS){
        resp=BUS_cmd_tx(SPI_addr,pk,1,0);
      }
      //check if command sent successfully
      if(resp!=RET_SUCCESS){
        //report error
        report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_SPI_COMPLETE_FAIL,resp);
      }
      //transaction complete, clear address
      SPI_addr=0;
    }
    if(e&BUS_HELPER_EV_SPI_CLEAR_CMD){
      //done with SPI send command
      BUS_cmd_init(pk,CMD_SPI_CLEAR);
      resp=BUS_cmd_tx(BUS_ADDR_CDH,pk,0,0);
      //check if command was successful and try again if it failed
      if(resp!=RET_SUCCESS){
        resp=BUS_cmd_tx(BUS_ADDR_CDH,pk,0,0);
      }
      //check if command sent successfully
      if(resp!=RET_SUCCESS){
        //report error
        report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_SPI_CLEAR_FAIL,resp);
      }
    }
    if(e&BUS_HELPER_EV_ASYNC_CLOSE){      
      //close async connection
      async_close_remote();
      //send event
      ctl_events_set_clear(&SUB_events,SUB_EV_ASYNC_CLOSE,0);
    }
    if(e&BUS_HELPER_EV_ERR_REQ){
        //get mutex
        if(ctl_mutex_lock(&err_req.mutex,CTL_TIMEOUT_DELAY,100)){
            //get buffer
            ptr=BUS_get_buffer(CTL_TIMEOUT_DELAY,100);
            //check if buffer was aquired
            if(ptr){
              //set data type
              ptr[0]=SPI_ERROR_DAT;
              //set own address
              ptr[1]=BUS_get_OA();
              //get maximum size for data packet. part of the buffer is used to read errors into
              maxsize=BUS_get_buffer_size()-512-2;
              //check if requested size is greater then max
              if(maxsize<err_req.size){
                //set maxsize
                err_req.size=maxsize;
              }
              //check request type
              switch(err_req.type){
                case ERR_REQ_REPLAY:
                  //get errors
                  error_log_mem_replay(ptr+2,err_req.size,err_req.level,ptr+2+maxsize);
                break;
              }
              //send data
              resp=BUS_SPI_txrx(err_req.dest,ptr,NULL,err_req.size+2);
              //Check if data was sent
              if(resp!=RET_SUCCESS){
                  //report error
                  report_error(ERR_LEV_ERROR,BUS_ERR_SRC_ERR_REQ,ERR_REQ_ERR_SPI_SEND,resp);
              }
              //free buffer
              BUS_free_buffer();
            }else{
              //set flag so we try again
              ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_ERR_REQ,0);
              //report error
              report_error(ERR_LEV_ERROR,BUS_ERR_SRC_ERR_REQ,ERR_REQ_ERR_BUFFER_BUSY,0);
            }
            ctl_mutex_unlock(&err_req.mutex);
        }else{
          //set flag so we try again
          ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_ERR_REQ,0);
          //report error
          report_error(ERR_LEV_ERROR,BUS_ERR_SRC_ERR_REQ,ERR_REQ_ERR_MUTEX_TIMEOUT,0);
        }
    }
    if(e&BUS_HELPER_EV_NACK){
      //double check address
      if(nack_info.addr){
        //send the command
        resp=BUS_cmd_tx(nack_info.addr,nack_info.dat,2,0);
        //check response
        if(resp!=RET_SUCCESS){
          //error sending packet, report error
          report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_TX_NACK_FAIL,resp);
        }
        //clear the address
        nack_info.addr=0;
      }else{
        //no nack packet? report error
        report_error(ERR_LEV_ERROR,BUS_ERR_SRC_MAIN_LOOP,MAIN_LOOP_ERR_UNEXPECTED_NACK_EV,0);
      }
    }
  }
}

//=======================================================================================
//                                 [Main Loop]
//=======================================================================================

//main loop function, start ARC_Bus task then enter Idle task
void mainLoop(void) __toplevel{
  //initialize events
  ctl_events_init(&BUS_INT_events,0);
  //start ARCbus task
  ctl_task_run(&ARC_bus_task,BUS_PRI_ARCBUS,ARC_bus_run,NULL,"ARC_Bus",sizeof(BUS_stack)/sizeof(BUS_stack[0])-2,BUS_stack+1,0);
  //kick WDT to give us some time
  WDT_KICK();
  // drop to lowest priority to start created tasks running.
  ctl_task_set_priority(&idle_task,0); 
  
  //main idle loop
  //NOTE that this task should never wait to ensure that there is always a runnable task
  for(;;){    
    //kick watchdog
    WDT_KICK();
    //go to low power mode
    LPM0;
  }
}

//main loop testing function, start ARC_Bus task then enter Idle task
void mainLoop_testing(void (*cb)(void)) __toplevel{
  //initialize events
  ctl_events_init(&BUS_INT_events,0);
  //start ARCbus task
  ctl_task_run(&ARC_bus_task,BUS_PRI_ARCBUS,ARC_bus_run,NULL,"ARC_Bus",sizeof(BUS_stack)/sizeof(BUS_stack[0])-2,BUS_stack+1,0);
  //kick WDT to give us some time
  WDT_KICK();
  // drop to lowest priority to start created tasks running.
  ctl_task_set_priority(&idle_task,0); 
  
  //main idle loop
  //NOTE that this task should never wait to ensure that there is always a runnable task
  for(;;){    
      //call the callback
      cb();
  }
}
