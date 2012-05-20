/*
    ChibiOS/RT - Copyright (C) 2006,2007,2008,2009,2010,
                 2011,2012 Giovanni Di Sirio.

    This file is part of ChibiOS/RT.

    ChibiOS/RT is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS/RT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ch.h"
#include "hal.h"

#include "ff.h"

#include "flashconfig.h"

#include "flash/ihex.h"
#include "flash/flash.h"
#include "flash/helper.h"

#define MMC_CONNECT_TIMEOUT_MS      500
#define MMC_FIRMWARE_FILENAME       "ch.hex"

#define BOOTLOADER_ERROR_MMC_NOCARD     1
#define BOOTLOADER_ERROR_MMC_BADFS      2
#define BOOTLOADER_ERROR_MMC_NOFILE     3
#define BOOTLOADER_ERROR_BAD_HEX        4
#define BOOTLOADER_ERROR_BAD_FLASH      5


/*===========================================================================*/
/* MMC/SPI related.                                                          */
/*===========================================================================*/

/**
 * @brief FS object.
 */
FATFS MMC_FS;

/**
 * @brief MMC driver instance.
 */
MMCDriver MMCD1;

/* Maximum speed SPI configuration (18MHz, CPHA=0, CPOL=0, MSb first).*/
static SPIConfig hs_spicfg = {NULL, IOPORT1, GPIOA_SPI3NSS, 0};

/* Low speed SPI configuration (281.250kHz, CPHA=0, CPOL=0, MSb first).*/
static SPIConfig ls_spicfg = {NULL, IOPORT1, GPIOA_SPI3NSS,
                              SPI_CR1_BR_2 | SPI_CR1_BR_1};

/* MMC/SD over SPI driver configuration.*/
static MMCConfig mmccfg = {&SPID3, &ls_spicfg, &hs_spicfg};

static void loaderError(unsigned int errno){
  unsigned int i;

  for (i=0; i< errno; i++) {
    palClearPad(GPIOC, GPIOC_LED_STATUS1);
    chThdSleepMilliseconds(500);
    palSetPad(GPIOC, GPIOC_LED_STATUS1);
    chThdSleepMilliseconds(500);
  }

  palClearPad(GPIOC, GPIOC_LED_STATUS1);


  flashJumpApplication(FLASH_USER_BASE);
}

/*===========================================================================*/
/* Flash programming                                                         */
/*===========================================================================*/

static bool_t flashing = FALSE;
static struct LinearFlashing flashPage;

static WORKING_AREA(waMMCFlasherThread, 2048);
static msg_t MMCFlasherThread(void *arg) {
  int err;
  EventListener elMMCinsert;
  (void)arg;

  chRegSetThreadName("MMCFlasherThread");

  /* Wait for card */
  chEvtRegister(&MMCD1.inserted_event, &elMMCinsert, 0);
  if(chEvtWaitOneTimeout(ALL_EVENTS, MS2ST(MMC_CONNECT_TIMEOUT_MS)) == 0)
    loaderError(BOOTLOADER_ERROR_MMC_NOCARD);

  if (mmcConnect(&MMCD1))
    loaderError(BOOTLOADER_ERROR_MMC_NOCARD);

  err = f_mount(0, &MMC_FS);
  if (err != FR_OK) {
    mmcDisconnect(&MMCD1);
    loaderError(BOOTLOADER_ERROR_MMC_BADFS);
  }

  FIL fp;
  if(f_open(&fp, MMC_FIRMWARE_FILENAME, FA_READ) != FR_OK)
    loaderError(BOOTLOADER_ERROR_MMC_NOFILE);

  /*
   * Here comes the flashing magic (pun intended).
   */
  flashing = TRUE;

  IHexRecord irec;
  uint16_t addressOffset = 0x00;
  uint32_t address = 0x0;
  enum IHexErrors ihexError;

  linearFlashProgramStart(&flashPage);

  while ((ihexError = Read_IHexRecord(&irec, &fp)) == IHEX_OK) {
    switch(irec.type){
      case IHEX_TYPE_00:    /**< Data Record */
        address = (((uint32_t) addressOffset) << 16 )+ irec.address;

        err = linearFlashProgram(&flashPage, address,
                                    (flashdata_t*) irec.data, irec.dataLen);

        if(err)
          loaderError(BOOTLOADER_ERROR_BAD_FLASH);

        break;

      case IHEX_TYPE_04:    /**< Extended Linear Address Record */
        addressOffset = (((uint16_t) irec.data[0]) << 8 ) + irec.data[1];
        break;

      case IHEX_TYPE_01:    /**< End of File Record */
      case IHEX_TYPE_05:    /**< Start Linear Address Record */
        break;

      case IHEX_TYPE_02:    /**< Extended Segment Address Record */
      case IHEX_TYPE_03:    /**< Start Segment Address Record */
        loaderError(BOOTLOADER_ERROR_BAD_HEX);
        break;
    }

  }

  err = linearFlashProgramFinish(&flashPage);

  f_close(&fp);

  flashing = FALSE;

  /* Remove firmware so that we do not reflash if something goes wrong */
  f_unlink(MMC_FIRMWARE_FILENAME);

  /* Wait for write action to have finished */
  chThdSleepMilliseconds(500);

  if(err)
    loaderError(BOOTLOADER_ERROR_BAD_FLASH);

  switch(ihexError){
    case IHEX_OK:
    case IHEX_ERROR_EOF:
      break;
    case IHEX_ERROR_FILE:
    case IHEX_ERROR_INVALID_RECORD:
    case IHEX_ERROR_INVALID_ARGUMENTS:
    case IHEX_ERROR_NEWLINE:
      loaderError(BOOTLOADER_ERROR_BAD_HEX);
      break;
  }



  /* Yeha, finished flashing and everything */
  flashJumpApplication(FLASH_USER_BASE);

  return CH_SUCCESS;
}


/*
 * Bootloader entry point.
 */
int main(void) {
  /*
   * System initializations.
   * - HAL initialization, this also initializes the configured device drivers
   *   and performs the board-specific initializations.
   * - Kernel initialization, the main() function becomes a thread and the
   *   RTOS is active.
   */
  halInit();
  chSysInit();

  /*
   * Notify user about entering the bootloader
   */
  palSetPad(GPIOC, GPIOC_LED_STATUS2);

  /*
   * Activates the serial driver 3 using the driver default configuration.
   */
  sdStart(&SD3, NULL);


  /*
   * Initializes the MMC driver to work with SPI3.
   */
  palSetPadMode(IOPORT1, GPIOA_SPI3NSS, PAL_MODE_OUTPUT_PUSHPULL);
  palSetPad(IOPORT1, GPIOA_SPI3NSS);
  mmcObjectInit(&MMCD1);
  mmcStart(&MMCD1, &mmccfg);

  /*
   * Creates the flasher thread.
   */
  chThdCreateStatic(waMMCFlasherThread, sizeof(waMMCFlasherThread),
      NORMALPRIO, MMCFlasherThread, NULL);

  while (TRUE) {
    /* Flash red LED depending on the flash status */
    if (flashing) {
      palClearPad(GPIOC, GPIOC_LED_STATUS2);
      chThdSleepMilliseconds(100);
      palSetPad(GPIOC, GPIOC_LED_STATUS2);
      chThdSleepMilliseconds(100);
    } else {
      chThdSleepMilliseconds(50);
    }

  }

  return 0;
}
