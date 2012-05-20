#include <string.h>

#include "ch.h"
#include "hal.h"
#include "flash/flash.h"


#define flashWaitWhileBusy() {while(FLASH->SR & FLASH_SR_BSY) {}}

/**
 * @brief Unlock the flash memory for write access.
 * @return  CH_SUCCESS  Unlock was successful.
 * @return CH_FAILED    Unlock failed.
 */
static bool_t flashUnlock(void){
  /* Check if unlock is really needed */
  if (!(FLASH->CR & FLASH_CR_LOCK))
    return CH_SUCCESS;

  /* Write magic unlock sequence */
  FLASH->KEYR = 0x45670123;
  FLASH->KEYR = 0xCDEF89AB;

  if (FLASH->CR & FLASH_CR_LOCK)
    return CH_FAILED;

  return CH_SUCCESS;
}

/**
 * @brief Lock the flash memory for write access.
 */
static void flashLock(void) {
  FLASH->CR |= FLASH_CR_LOCK;
}


int flashPageErase(flashpage_t page){

  /* Only write on pages in the user area */
  if (!(FLASH_IS_ADDRESS_USERSPACE(FLASH_ADDRESS_OF_PAGE(page))))
    return FLASH_RETURN_NO_PERMISSION;

  /* Unlock flash for write access */
  if(flashUnlock() == CH_FAILED)
    return FLASH_RETURN_NO_PERMISSION;

  /* Wait for any busy flags. */
  flashWaitWhileBusy();

  /* Start deletion of page. */
  FLASH->CR |= FLASH_CR_PER;
  FLASH->AR = FLASH_ADDRESS_OF_PAGE(page);
  FLASH->CR |= FLASH_CR_STRT;

  /* Wait until it's finished. */
  flashWaitWhileBusy();

  /* Page erase flag does not clear automatically. */
  FLASH->CR &= !FLASH_CR_PER;

  /* Lock flash again */
  flashLock();

  /* Check deleted page for errors */
  if(flashPageCheckErased(page) == FALSE)
    return FLASH_RETURN_BADFLASH;  /* Page is not empty despite the erase cycle! */

  /* Successfully deleted page */
  return FLASH_RETURN_SUCCESS;
}

bool_t flashPageCheckErased(flashpage_t page){
  uint32_t* const startAddress = (uint32_t*) FLASH_ADDRESS_OF_PAGE(page);
  uint32_t* const stopAddress = (uint32_t*) FLASH_ADDRESS_OF_PAGE(page+1);

  uint32_t* addr;

  /* Cycle through the whole page and check for default set bits */
  for(addr = startAddress; addr < stopAddress; addr++){
    if (*addr != 0xffffffff)
      return FALSE;
  }

  /* The whole page was empty */
  return TRUE;
}


bool_t flashPageRead(flashpage_t page, flashdata_t* buffer){
  memcpy(buffer, (void*) FLASH_ADDRESS_OF_PAGE(page), FLASH_PAGE_SIZE);
  return CH_SUCCESS;
}

int flashPageCompare(flashpage_t page, const flashdata_t* buffer){
  const uint32_t* const pageAddr = (const uint32_t*) FLASH_ADDRESS_OF_PAGE(page);
  const uint32_t* const bufferAddr = (const uint32_t*) buffer;

  unsigned int pos;
  bool_t identical = TRUE;

  for(pos = 0; pos < FLASH_PAGE_SIZE / sizeof(uint32_t); pos++) {

    if (pageAddr[pos] == bufferAddr[pos]) {
      continue;
    }

    /* Keep track if the buffer is identical to page -> mark not identical*/
    if (identical) {
      identical = FALSE;
    }

    /* Not identical, and not erased, needs erase*/
    if (pageAddr[pos] != 0xffffffff)
      return 2;
  }

  /* Page is not identical, but no page erase is needed to write. */
  if (!identical)
    return 1;

  /* Page is identical. No write is needed. */
  return 0;
}


int flashPageWrite(flashpage_t page, const flashdata_t* buffer){
  volatile flashdata_t* const pageAddr =
                                     (flashdata_t*) FLASH_ADDRESS_OF_PAGE(page);

  /* Only write on pages in the user area */
  if (!(FLASH_IS_ADDRESS_USERSPACE(FLASH_ADDRESS_OF_PAGE(page))))
    return FLASH_RETURN_NO_PERMISSION;

  unsigned int pos;

  /* Unlock flash for write access */
  if(flashUnlock() == CH_FAILED)
    return FLASH_RETURN_NO_PERMISSION;

  flashWaitWhileBusy();

  for(pos = 0; pos < FLASH_PAGE_SIZE / sizeof(flashdata_t); pos++) {

    /* Enter flash programming mode. */
    FLASH->CR |= FLASH_CR_PG;

    /* Write half-word to flash. */
    pageAddr[pos] = buffer[pos];

    /* Wait for completion */
    flashWaitWhileBusy();

    /* Exit flash programming mode. */
    FLASH->CR &= ~FLASH_CR_PG;

    /* Check for flash error. */

    if (pageAddr[pos] != buffer[pos])
      return FLASH_RETURN_BADFLASH;
  }

  flashLock();

  return 0;
}


int flashPageWriteIfNeeded(flashpage_t page, const flashdata_t* buffer){
  int err;

  /* Only write on pages in the user area */
  if (!(FLASH_IS_ADDRESS_USERSPACE(FLASH_ADDRESS_OF_PAGE(page))))
    return FLASH_RETURN_NO_PERMISSION;

  err = flashPageCompare(page, buffer);

  /* Don't do anything in case of error or if pages are identical */
  if (err <= 0)
    return err;

  /* Page needs erase */
  if (err == 2)
    err = flashPageErase(page);

  /* Return errors of page erase */
  if (err != FLASH_RETURN_SUCCESS)
    return err;

  err = flashPageWrite(page, buffer);

  return err;
}
