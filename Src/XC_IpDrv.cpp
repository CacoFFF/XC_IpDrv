/*=============================================================================
	XC_Drv.cpp
	Package and UClasses implementations.
=============================================================================*/

#include "XC_IpDrv.h"
#include "Cacus/DebugCallback.h"

/*-----------------------------------------------------------------------------
	Declarations.
-----------------------------------------------------------------------------*/

// Register things.
#define NAMES_ONLY
#define AUTOGENERATE_NAME(name) FName XC_IPDRV_##name;
#define AUTOGENERATE_FUNCTION(cls,idx,name) IMPLEMENT_FUNCTION(cls,idx,name)
#include "XC_IpDrvClasses.h"
#undef AUTOGENERATE_FUNCTION
#undef AUTOGENERATE_NAME
#undef NAMES_ONLY
static inline void RegisterNames()
{
	static INT Registered=0;
	if(!Registered++)
	{
		#define NAMES_ONLY
		#define AUTOGENERATE_NAME(name) extern XC_IPDRV_API FName XC_IPDRV_##name; XC_IPDRV_##name=FName(TEXT(#name),FNAME_Intrinsic);
		#define AUTOGENERATE_FUNCTION(cls,idx,name)
		#include "XC_IpDrvClasses.h"
		#undef DECLARE_NAME
		#undef NAMES_ONLY
	}
}


IMPLEMENT_PACKAGE(XC_IpDrv);

/*-----------------------------------------------------------------------------
	Definitions.
-----------------------------------------------------------------------------*/

// Get local IP to bind to
TArray<IPAddress> GetLocalBindAddress( FOutputDevice& Out)
{
	UBOOL bCanBindAll;

	TArray<IPAddress> HostAddr = GetLocalHostAddress( Out, bCanBindAll);
	if ( bCanBindAll )
	{
		HostAddr.Empty();
		HostAddr.AddItem(IPAddress::Any);
		if ( GIPv6 ) // Also add IPv4 ANY
			HostAddr.AddItem(IPAddress(0,0,0,0));
	}
	return HostAddr;
}

//Should export as C
TArray<IPAddress> GetLocalHostAddress( FOutputDevice& Out, UBOOL& bCanBindAll)
{
	guard(GetLocalHostAddress);

	TArray<IPAddress> Addresses;
	IPAddress HostAddr = IPAddress::Any;
	TCHAR Home[256] = TEXT("");
	bCanBindAll = false;

	// TODO: Bind to multiple addresses
	if ( Parse( appCmdLine(), TEXT("MULTIHOME="), Home, ARRAY_COUNT(Home)) )
	{
		HostAddr = CSocket::ResolveHostname( appToAnsi(Home));
		if ( HostAddr==IPAddress::Any )
			Out.Logf( TEXT("Invalid multihome IP address %s"), Home);
		//else
		//	Out.Logf( NAME_Init, TEXT("%s: Multihome %s resolved to (%s)"), FSocket::API, Home, *HostAddr.String() );
	}
	else
	{
		FString Hostname( CSocket::GetHostname() );
		bCanBindAll = !ParseParam(appCmdLine(),TEXT("PRIMARYNET"));
		if ( !bCanBindAll )
			HostAddr = CSocket::ResolveHostname(""); //get local host name

		static uint32 First = 0;
		if( !First )
		{
			First = 1;
			Out.Logf( NAME_Init, TEXT("%s: I am %s (%s)"), appFromAnsi(CSocket::API), *Hostname, appFromAnsi(*HostAddr) );
		}
	}
	Addresses.AddItem(HostAddr);
	return Addresses;
	unguard;
}

/*----------------------------------------------------------------------------
	Non-blocking resolver.
----------------------------------------------------------------------------*/

// This temporarily overrides XC_Engine's logger
void ResolveExceptionCallback( const char* Message, int MessageFlags)
{
	throw Message;
}

// Resolution thread entrypoint.
unsigned long ResolveThreadEntry( void* Arg, CThread* Handler)
{
	FResolveInfo* Info = (FResolveInfo*)Arg;
	CDbg_RegisterCallback( &ResolveExceptionCallback, CACUS_CALLBACK_NET|CACUS_CALLBACK_EXCEPTION, 0);
	// Awful Java styled code
	try
	{
		Info->Addr = CSocket::ResolveHostname( Info->HostName, false, true);
	}
	catch ( const char* Message )
	{
		appFromAnsiInPlace( Info->Error, Message);
	}
	CDbg_UnregisterCallback( &ResolveExceptionCallback);
	return THREAD_END_OK;
}

FResolveInfo::FResolveInfo( const TCHAR* InHostName )
	: CThread()
{
	debugf( TEXT("[XC] Resolving %s..."), InHostName );

	appToAnsiInPlace( HostName, InHostName);
	Error[0] = '\0';

	Run( &ResolveThreadEntry, this);
}

int32 FResolveInfo::Resolved()
{
	return WaitFinish( 0.001f);
}
const TCHAR* FResolveInfo::GetError() const
{
	return *Error ? Error : NULL;
}



/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

