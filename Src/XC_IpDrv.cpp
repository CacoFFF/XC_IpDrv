/*=============================================================================
	XC_Drv.cpp
	Package and UClasses implementations.
=============================================================================*/

#include "XC_IpDrv.h"
#include "Cacus/DebugCallback.h"
#include "Cacus/CacusString.h"

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
	TArray<IPAddress> Addresses = GetLocalHostAddress( Out, bCanBindAll);
	if ( bCanBindAll && GIPv6 )
	{
		// Also add IPv4 ANY
		INT Index;
		if ( Addresses.FindItem( IPAddress::Any, Index) )
			Addresses.InsertItem( Index+1, IPAddress(0,0,0,0));
	}
	return Addresses;
}

//Should export as C
static uint32 First = 0;
TArray<IPAddress> GetLocalHostAddress( FOutputDevice& Out, UBOOL& bCanBindAll)
{
	guard(GetLocalHostAddress);

	TArray<IPAddress> Addresses;

	const TCHAR* MultiHome = appStrfind( appCmdLine(), TEXT("MULTIHOME="));
	if ( MultiHome )
	{
		FMemMark Mark(GMem);

		const TCHAR* MultiHomeEnd = (MultiHome += _len("MULTIHOME="));
		AdvanceTo( MultiHomeEnd, TEXT("\" \r\n"));
		size_t MultiHomeSize = 1 + MultiHomeEnd - MultiHome;
		TCHAR* Start = new(GMem,MultiHomeSize) TCHAR;
		CStrcpy_s( Start, MultiHomeSize, MultiHome);

		while ( true )
		{
			const TCHAR* End = Start;
			AdvanceTo( End, TEXT(",;"));
			bool bLast = (*End != ',') && (*End != ';');
			*(TCHAR*)End = '\0';

			// Get address
			if ( !appStricmp(Start,TEXT("PRIMARYNET")) )
				Addresses.AddUniqueItem(CSocket::ResolveHostname(""));
			else if ( !appStricmp(Start,TEXT("ALL")) || !appStricmp(Start,TEXT("ANY")) )
				Addresses.AddUniqueItem(IPAddress::Any);
			else
			{
				IPAddress Resolved = CSocket::ResolveHostname(appToAnsi(Start));
				if ( Resolved != IPAddress::Any )
					Addresses.AddUniqueItem(Resolved);
			}

			if ( bLast )
				break;
			Start = (TCHAR*)End + 1;
		}
		Mark.Pop();

		if ( Addresses.Num() )
		{
			if ( !First )
			{
				First = 1;
				FString API(CSocket::API); //ANSICHAR constructor
				for ( int32 i=0; i<Addresses.Num(); i++)
					Out.Logf( NAME_Init, TEXT("%s: MultiHome address %i is %s"), *API, i+1, *FString(*Addresses(i)) );
			}
			bCanBindAll = Addresses.FindItemIndex(IPAddress::Any) != INDEX_NONE;
			return Addresses;
		}
	}

	IPAddress HostAddr = IPAddress::Any;
	FString Hostname( CSocket::GetHostname() );
	bCanBindAll = !ParseParam(appCmdLine(),TEXT("PRIMARYNET"));
	if ( !bCanBindAll )
		HostAddr = CSocket::ResolveHostname(""); //get local host name

	if ( !First )
	{
		First = 1;
		Out.Logf( NAME_Init, TEXT("%s: I am %s (%s)"), appFromAnsi(CSocket::API), *Hostname, appFromAnsi(*HostAddr) );
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

