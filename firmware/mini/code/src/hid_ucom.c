/*
 * @brief HID USB Comm port call back routines
 *
 * @note
 * Copyright(C) NXP Semiconductors, 2012
 * All rights reserved.
 *
 * @par
 * Software that is described herein is for illustrative purposes only
 * which provides customers with programming information regarding the
 * LPC products.  This software is supplied "AS IS" without any warranties of
 * any kind, and NXP Semiconductors and its licensor disclaim any and
 * all warranties, express or implied, including all implied warranties of
 * merchantability, fitness for a particular purpose and non-infringement of
 * intellectual property rights.  NXP Semiconductors assumes no responsibility
 * or liability for the use of the software, conveys no license or rights under any
 * patent, copyright, mask work right, or any other intellectual property rights in
 * or to any products. NXP Semiconductors reserves the right to make changes
 * in the software without notification. NXP Semiconductors also makes no
 * representation or warranty that such application will be suitable for the
 * specified use without further testing or modification.
 *
 * @par
 * Permission to use, copy, modify, and distribute this software and its
 * documentation is hereby granted, under NXP Semiconductors' and its
 * licensor's relevant copyrights in the software, without fee, provided that it
 * is used in conjunction with NXP Semiconductors microcontrollers.  This
 * copyright, permission, and disclaimer notice must appear in all copies of
 * this code.
 */
#include <string.h>
#include "board.h"
#include "app_usbd_cfg.h"
#include "hid_ucom.h"

#include "protocol.h"
#include "defines.h"
#include "avalon_a3222.h"

/*****************************************************************************
 * Private types/enumerations/variables
 ****************************************************************************/
/* Ring buffer size */
#define TX_BUF_CNT          4
#define RX_BUF_CNT          8
#define UCOM_TX_BUF_SZ		(TX_BUF_CNT * AVAM_P_COUNT)
#define UCOM_RX_BUF_SZ		(RX_BUF_CNT * AVAM_P_COUNT)

#define UCOM_TX_CONNECTED   _BIT(8)
#define UCOM_TX_BUSY        _BIT(0)
#define UCOM_RX_BUF_FULL    _BIT(1)
#define UCOM_RX_BUF_QUEUED  _BIT(2)

static RINGBUFF_T usb_rxrb, usb_txrb;
static uint8_t usb_rxdata[RX_BUF_CNT * AVAM_P_COUNT], usb_txdata[TX_BUF_CNT * AVAM_P_COUNT];

/**
 * Structure containing Virtual Comm port control data
 */
typedef struct UCOM_DATA {
	USBD_HANDLE_T hUsb;		/*!< Handle to USB stack */
	uint16_t usbRx_count;
	uint8_t *usbTx_buff;
	uint8_t *usbRx_buff;
	volatile uint16_t usbTxFlags;	/*!< USB Tx Flag */
	volatile uint16_t usbRxFlags;	/*!< USB Rx Flag */
} UCOM_DATA_T;

/** Virtual Comm port control data instance. */
static UCOM_DATA_T g_usb;

/*****************************************************************************
 * Public types/enumerations/variables
 ****************************************************************************/
extern const uint8_t UCOM_ReportDescriptor[];
extern const uint16_t UCOM_ReportDescSize;

/*****************************************************************************
 * Private functions
 ****************************************************************************/
static void UCOM_BufInit(void)
{
	RingBuffer_Init(&usb_rxrb, usb_rxdata, AVAM_P_COUNT, RX_BUF_CNT);
	RingBuffer_Init(&usb_txrb, usb_txdata, AVAM_P_COUNT, TX_BUF_CNT);
	g_usb.usbTxFlags |= UCOM_TX_CONNECTED;
}

/* HID Get Report Request Callback. Called automatically on HID Get Report Request */
static ErrorCode_t UCOM_GetReport(USBD_HANDLE_T hHid, USB_SETUP_PACKET *pSetup, uint8_t * *pBuffer, uint16_t *plength)
{
	return LPC_OK;
}

/* HID Set Report Request Callback. Called automatically on HID Set Report Request */
static ErrorCode_t UCOM_SetReport(USBD_HANDLE_T hHid, USB_SETUP_PACKET *pSetup, uint8_t * *pBuffer, uint16_t length)
{
	return LPC_OK;
}

/* UCOM interrupt EP_IN and EP_OUT endpoints handler */
static ErrorCode_t UCOM_int_hdlr(USBD_HANDLE_T hUsb, void *data, uint32_t event)
{
	switch (event) {
	case USB_EVT_IN:
		/* USB_EVT_IN occurs when HW completes sending IN packet. So clear the
		    busy flag for main loop to queue next packet.
		 */
		g_usb.usbTxFlags &= ~UCOM_TX_BUSY;
		if (RingBuffer_GetCount(&usb_txrb) >= 1) {
			g_usb.usbTxFlags |= UCOM_TX_BUSY;
			RingBuffer_Pop(&usb_txrb, g_usb.usbTx_buff);
			USBD_API->hw->WriteEP(g_usb.hUsb, HID_EP_IN, g_usb.usbTx_buff, AVAM_P_COUNT);
		}
		break;
	case USB_EVT_OUT:
		g_usb.usbRx_count = USBD_API->hw->ReadEP(hUsb, HID_EP_OUT, g_usb.usbRx_buff);
#ifdef DEBUG_VERBOSE
		if (RingBuffer_GetCount(&usb_rxrb) == RX_BUF_CNT) {
			debug32("E:(%d-%x %x %x %x) usb_rxrb overflow evt out\n", g_usb.usbRx_count,
					g_usb.usbRx_buff[0],
					g_usb.usbRx_buff[1],
					g_usb.usbRx_buff[2],
					g_usb.usbRx_buff[3]);
		}
#endif

		if (g_usb.usbRx_count >= AVAM_P_COUNT) {
			RingBuffer_Insert(&usb_rxrb, g_usb.usbRx_buff);
			g_usb.usbRx_count -= AVAM_P_COUNT;
		}

		if (g_usb.usbRxFlags & UCOM_RX_BUF_QUEUED) {
			g_usb.usbRxFlags &= ~UCOM_RX_BUF_QUEUED;
			if (g_usb.usbRx_count != 0)
				g_usb.usbRxFlags |= UCOM_RX_BUF_FULL;
		}
		break;
	case USB_EVT_OUT_NAK:
		/* queue free buffer for RX */
		if ((g_usb.usbRxFlags & (UCOM_RX_BUF_FULL | UCOM_RX_BUF_QUEUED)) == 0) {
			g_usb.usbRx_count = USBD_API->hw->ReadReqEP(hUsb, HID_EP_OUT, g_usb.usbRx_buff, UCOM_RX_BUF_SZ);
#ifdef DEBUG_VERBOSE
			if (RingBuffer_GetCount(&usb_rxrb) == RX_BUF_CNT)
				debug32("E: usb_rxrb overflow evt nak\n");
#endif
			if (g_usb.usbRx_count >= AVAM_P_COUNT) {
				RingBuffer_Insert(&usb_rxrb, g_usb.usbRx_buff);
				g_usb.usbRx_count -= AVAM_P_COUNT;
			}
			g_usb.usbRxFlags |= UCOM_RX_BUF_QUEUED;
		}
		break;
	default:
		break;
	}

	return LPC_OK;
}

/* USB com port init routine */
ErrorCode_t UCOM_init(USBD_HANDLE_T hUsb, USB_INTERFACE_DESCRIPTOR *pIntfDesc, USBD_API_INIT_PARAM_T *pUsbParam)
{
	USBD_HID_INIT_PARAM_T hid_param;
	USB_HID_REPORT_T reports_data[1];
	ErrorCode_t ret = LPC_OK;

	/* Store USB stack handle for future use. */
	g_usb.hUsb = hUsb;
	/* Initi CDC params */
	memset((void *) &hid_param, 0, sizeof(USBD_HID_INIT_PARAM_T));
	hid_param.max_reports = 1;
	hid_param.mem_base = pUsbParam->mem_base;
	hid_param.mem_size = pUsbParam->mem_size;
	hid_param.intf_desc = (uint8_t *)pIntfDesc;
	hid_param.HID_GetReport = UCOM_GetReport;
	hid_param.HID_SetReport = UCOM_SetReport;
	hid_param.HID_EpIn_Hdlr = UCOM_int_hdlr;
	hid_param.HID_EpOut_Hdlr = UCOM_int_hdlr;
	reports_data[0].len = UCOM_ReportDescSize;
	reports_data[0].idle_time = 0;
	reports_data[0].desc = (uint8_t *) &UCOM_ReportDescriptor[0];
	hid_param.report_data  = reports_data;

	/* Init HID interface */
	ret = USBD_API->hid->init(hUsb, &hid_param);

	if (ret == LPC_OK) {
		/* allocate transfer buffers */
		g_usb.usbRx_buff = (uint8_t *)hid_param.mem_base;
		hid_param.mem_base += UCOM_RX_BUF_SZ;
		hid_param.mem_size -= UCOM_RX_BUF_SZ;

		g_usb.usbTx_buff = (uint8_t *)hid_param.mem_base;
		hid_param.mem_base += UCOM_TX_BUF_SZ;
		hid_param.mem_size -= UCOM_TX_BUF_SZ;

		UCOM_BufInit();

		/* update mem_base and size variables for cascading calls. */
		pUsbParam->mem_base = hid_param.mem_base;
		pUsbParam->mem_size = hid_param.mem_size;
	}

	return ret;
}

/* Gets current read count. */
uint32_t UCOM_Read_Cnt(void)
{
	return RingBuffer_GetCount(&usb_rxrb);
}

/* Read data from usb */
uint32_t UCOM_Read(uint8_t *pBuf)
{
	uint16_t cnt = 0;

	cnt = RingBuffer_Pop(&usb_rxrb, (uint8_t *) pBuf);
	g_usb.usbRxFlags &= ~UCOM_RX_BUF_FULL;

	return cnt;
}

/* Send data to usb */
uint32_t UCOM_Write(uint8_t *pBuf)
{
	uint32_t ret = 0;

	RingBuffer_Insert(&usb_txrb, pBuf);

	if (g_usb.usbTxFlags & UCOM_TX_CONNECTED) {
		if (!(g_usb.usbTxFlags & UCOM_TX_BUSY) && RingBuffer_GetCount(&usb_txrb)) {
			g_usb.usbTxFlags |= UCOM_TX_BUSY;
			RingBuffer_Pop(&usb_txrb, g_usb.usbTx_buff);
			ret = USBD_API->hw->WriteEP(g_usb.hUsb, HID_EP_IN, g_usb.usbTx_buff, AVAM_P_COUNT);
		}
	}

	return ret;
}

void UCOM_Flush(void)
{
	RingBuffer_Flush(&usb_txrb);
	RingBuffer_Flush(&usb_rxrb);
}
