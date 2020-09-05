/*=============================================================================
	XC_IpDrv.h
=============================================================================*/

#ifndef XC_IPDRV_H
#define XC_IPDRV_H

#pragma once

#define CACUS_API DLL_IMPORT
#define XC_CORE_API DLL_IMPORT
#define XC_IPDRV_API DLL_EXPORT

#if _MSC_VER
	#pragma warning( disable : 4201 )
#endif

#include "Engine.h"
#include "UnNet.h"

#include "Cacus/CacusThread.h"
#include "Cacus/CacusPlatform.h"
#include "Cacus/IPv6.h"
#include "Cacus/NetworkSocket.h"

#include "XC_Template.h"

/*----------------------------------------------------------------------------
	Non-blocking resolver.
----------------------------------------------------------------------------*/

unsigned long ResolveThreadEntry( void* Arg, struct CThread* Handler);

class FResolveInfo : public CThread
{
public:
	// Variables.
	IPAddress Addr;
	TCHAR Error[256];
	ANSICHAR HostName[256];

	// Functions.
	FResolveInfo( const TCHAR* InHostName );

	int32 Resolved();
	const TCHAR* GetError() const; //Returns nullptr in absence of error
};


/*----------------------------------------------------------------------------
	Functions.
----------------------------------------------------------------------------*/

TArray<IPAddress> GetLocalBindAddress( FOutputDevice& Out);
TArray<IPAddress> GetLocalHostAddress( FOutputDevice& Out, UBOOL& bCanBindAll);

#include "XC_DownloadURL.h"
#include "XC_IpDrvClasses.h"
#include "XC_TcpNetDriver.h"

#endif

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

