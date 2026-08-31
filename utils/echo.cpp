#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <libftdi1/ftdi.h>


int main()
{
   struct ftdi_context *ftdi;
   int ret;

   // create ftdi context struct (c library)
   if((ftdi = ftdi_new()) == 0)
   {
      throw std::runtime_error("Could not create libftdi context");
   }

   //if((ret = ftdi_usb_open(ftdi, 0x2a19, 0x1009)) < 0)
   if((ret = ftdi_usb_open(ftdi, 0x0403, 0x6014)) < 0)
   {
      char err[1000] = "libftdi usb open error: ";
      const char *ftdi_err = ftdi_get_error_string(ftdi);
      strcat(err,ftdi_err);
      ftdi_free(ftdi);
      throw std::runtime_error(err);
   }

   // set latency timer
   if (ftdi_set_latency_timer(ftdi, 100) < 0)
   {
      fprintf(stderr,"Can't set latency timer: %s\n",ftdi_get_error_string(ftdi));
      exit(0);
   }

   // set baudrate
   ftdi_set_baudrate(ftdi, 3000000);
   printf("real baudrate used: %d\n", ftdi->baudrate);

   // Set Bitmode
   if (ftdi_set_bitmode(ftdi, 0xFF,BITMODE_RESET) < 0)
   {
      fprintf(stderr,"Can't set mode: %s\n",ftdi_get_error_string(ftdi));
      exit(0);
   }

   // Turn off flow control
   // ftdi_setflowctrl(ftdi, 1);

   ////Bitmode Reset
   //ret = ftdi_set_bitmode(ftdi, 0xFF, BITMODE_RESET);
   //if (ret != 0) {
   //   ftdi_usb_close(ftdi);
   //   printf("unable to RESET bitmode of Tricorder\n");
   //   exit(0);
   //}

   ////Set FT 245 Synchronous FIFO mode
   //ret = ftdi_set_bitmode(ftdi, 0xFF, BITMODE_SYNCFF);
   //if (ret != 0) {
   //   ftdi_usb_close(ftdi);
   //   printf("unable to set synchronous FIFO mode of Tricorder\n");
   //   exit(0);
   //}

   // purge USB buffers on FT chip
   ftdi_tcioflush(ftdi);

   std::string str = "Hello World";
   unsigned char buf_send[8192];
   buf_send[0] = 0x08;
   buf_send[1] = 0x0a;

   printf("Wrote %d bytes\n", ftdi_write_data(ftdi, buf_send, 2));

   unsigned char buf[8192];
   int bytes = ftdi_read_data(ftdi, buf, 1);

   printf("Read %d bytes\n", bytes);
   printf("Read \"%x\"\n\n", *buf);

   if(ftdi != nullptr) ftdi_free(ftdi);

}


