/*
 * Copyright (C) 1994-1998 T. Teranishi
 * (C) 2006-2017 TeraTerm Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* TERATERM.EXE, main */

#include "stdafx.h"
#include "teraterm.h"
#include "tttypes.h"
#include "commlib.h"
#include "ttwinman.h"
#include "buffer.h"
#include "vtterm.h"
#include "vtwin.h"
#include "clipboar.h"
#include "ttftypes.h"
#include "filesys.h"
#include "telnet.h"
#include "tektypes.h"
#include "tekwin.h"
#include "ttdde.h"
#include "keyboard.h"

#include "teraapp.h"

#include "compat_w95.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

BEGIN_MESSAGE_MAP(CTeraApp, CWinApp)
	//{{AFX_MSG_MAP(CTeraApp)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

CTeraApp::CTeraApp()
{
	typedef BOOL (WINAPI *pSetDllDir)(LPCSTR);
	typedef BOOL (WINAPI *pSetDefDllDir)(DWORD);

	HMODULE module;
	pSetDllDir setDllDir;
	pSetDefDllDir setDefDllDir;

	if ((module = GetModuleHandle("kernel32.dll")) != NULL) {
		if ((setDefDllDir = (pSetDefDllDir)GetProcAddress(module, "SetDefaultDllDirectories")) != NULL) {
			// SetDefaultDllDirectories() が使える場合は、検索パスを %WINDOWS%\system32 のみに設定する
			(*setDefDllDir)((DWORD)0x00000800); // LOAD_LIBRARY_SEARCH_SYSTEM32
		}
		else if ((setDllDir = (pSetDllDir)GetProcAddress(module, "SetDllDirectoryA")) != NULL) {
			// SetDefaultDllDirectories() が使えなくても、SetDllDirectory() が使える場合は
			// カレントディレクトリだけでも検索パスからはずしておく。
			(*setDllDir)("");
		}
	}
}

// CTeraApp instance
CTeraApp theApp;





// CTeraApp initialization
BOOL CTeraApp::InitInstance()
{
	hInst = m_hInstance;
	m_pMainWnd = new CVTWindow();
	pVTWin = m_pMainWnd;
	return TRUE;
}

int CTeraApp::ExitInstance()
{
	return CWinApp::ExitInstance();
}

// Tera Term main engine
BOOL CTeraApp::OnIdle(LONG lCount)
{
	static int Busy = 2;
	int Change, nx, ny;
	BOOL Size;

	if (lCount==0) Busy = 2;

	if (cv.Ready)
	{
		/* Sender */
		CommSend(&cv);

		/* Parser */
		if ((cv.HLogBuf!=NULL) && (cv.LogBuf==NULL))
			cv.LogBuf = (PCHAR)GlobalLock(cv.HLogBuf);

		if ((cv.HBinBuf!=NULL) && (cv.BinBuf==NULL))
			cv.BinBuf = (PCHAR)GlobalLock(cv.HBinBuf);

		if ((TelStatus==TelIdle) && cv.TelMode)
			TelStatus = TelIAC;

		if (TelStatus != TelIdle)
		{
			ParseTel(&Size,&nx,&ny);
			if (Size) {
				LockBuffer();
				ChangeTerminalSize(nx,ny);
				UnlockBuffer();
			}
		}
		else {
			if (cv.ProtoFlag) Change = ProtoDlgParse();
			else {
				switch (ActiveWin) {
				case IdVT:
					Change =  ((CVTWindow*)pVTWin)->Parse();
					// TEK windowのアクティブ中に pause を使うと、CPU使用率100%となる
					// 現象への暫定対処。(2006.2.6 yutaka)
					// 待ち時間をなくし、コンテキストスイッチだけにする。(2006.3.20 yutaka)
					Sleep(0);
					break;

				case IdTEK:
					if (pTEKWin != NULL) {
						Change = ((CTEKWindow*)pTEKWin)->Parse();
						// TEK windowのアクティブ中に pause を使うと、CPU使用率100%となる
						// 現象への暫定対処。(2006.2.6 yutaka)
						Sleep(1);
					}
					else {
						Change = IdVT;
					}
					break;

				default:
					Change = 0;
				}

				switch (Change) {
					case IdVT:
						VTActivate();
						break;
					case IdTEK:
						((CVTWindow*)pVTWin)->OpenTEK();
						break;
				}
			}
		}

		if (cv.LogBuf!=NULL)
		{
			if (FileLog) {
				LogToFile();
			}
			if (DDELog && AdvFlag) {
				DDEAdv();
			}
			GlobalUnlock(cv.HLogBuf);
			cv.LogBuf = NULL;
		}

		if (cv.BinBuf!=NULL)
		{
			if (BinLog) {
				LogToFile();
			}
			GlobalUnlock(cv.HBinBuf);
			cv.BinBuf = NULL;
		}

		/* Talker */
		switch (TalkStatus) {
		case IdTalkCB:
			CBSend();
			break; /* clip board */
		case IdTalkFile:
			FileSend();
			break; /* file */
		}

		/* Receiver */
		if (DDELog && cv.DCount >0) {
			// ログバッファがまだDDEクライアントへ送られていない場合は、
			// TCPパケットの受信を行わない。
			// 連続して受信を行うと、ログバッファがラウンドロビンにより未送信のデータを
			// 上書きしてしまう可能性がある。(2007.6.14 yutaka)

		} else {
			CommReceive(&cv);
		}

	}

	if (cv.Ready &&
	    (cv.RRQ || (cv.OutBuffCount>0) || (cv.InBuffCount>0) || (cv.FlushLen>0) || (cv.LCount>0) || (cv.BCount>0) || (cv.DCount>0)) ) {
		Busy = 2;
	}
	else {
		Busy--;
	}

	return (Busy>0);
}

BOOL CTeraApp::PreTranslateMessage(MSG* pMsg)
{
	if (MetaKey(ts.MetaKey)) {
		return FALSE; /* ignore accelerator keys */
	}
	else {
		return CWinApp::PreTranslateMessage(pMsg);
	}
}
