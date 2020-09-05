/*=============================================================================
	XC_TcpNetDriver.h
	Author: Fernando Velazquez

	Unreal Tournament IPv6 capable net driver.
=============================================================================*/


/*-----------------------------------------------------------------------------
	UXC_TcpipConnection.
-----------------------------------------------------------------------------*/

#include "UnNet.h"

//
// Windows socket class.
//
class UXC_TcpipConnection : public UNetConnection
{
	DECLARE_CLASS(UXC_TcpipConnection,UNetConnection,CLASS_Config|CLASS_Transient,XC_IpDrv)
	NO_DEFAULT_CONSTRUCTOR(UXC_TcpipConnection)

	// Variables.
	IPEndpoint	RemoteAddress;
	CSocket			Socket;
	UBOOL			OpenedLocally;
	FResolveInfo*	ResolveInfo;

	// Constructors and destructors.
	UXC_TcpipConnection( CSocket InSocket, UNetDriver* InDriver, IPEndpoint InRemoteAddress, EConnectionState InState, UBOOL InOpenedLocally, const FURL& InURL );

	void LowLevelSend( void* Data, INT Count );
	FString LowLevelGetRemoteAddress();
	FString LowLevelDescribe();
};

/*-----------------------------------------------------------------------------
	UXC_TcpNetDriver.
-----------------------------------------------------------------------------*/

class UXC_TcpNetDriver : public UNetDriver
{
	DECLARE_CLASS(UXC_TcpNetDriver,UNetDriver,CLASS_Transient|CLASS_Config,XC_IpDrv)
	NO_DEFAULT_CONSTRUCTOR(UXC_TcpNetDriver)

	UBOOL AllowPlayerPortUnreach;
	UBOOL LogPortUnreach;
	UBOOL RedirectInternal;
	UBOOL UseIPv6;
	int32 RedirectRate; //Not implemented
	int32 RedirectPort; //Not implemented
	int32 ConnectionLimit;

	// Variables.
	IPEndpoint LocalAddress;
	TArray<CSocket> Sockets;
//	CSocket Socket;

	// Constructor.
	void StaticConstructor();

	// UObject interface
	void PostEditChange();

	// UNetDriver interface.
	UBOOL InitConnect( FNetworkNotify* InNotify, FURL& ConnectURL, FString& Error );
	UBOOL InitListen( FNetworkNotify* InNotify, FURL& LocalURL, FString& Error );
	void TickDispatch( FLOAT DeltaTime );
	FString LowLevelGetNetworkNumber();
	void LowLevelDestroy();

	// UTcpNetDriver interface.
	UBOOL InitBase( UBOOL Connect, FNetworkNotify* InNotify, FURL& URL, FString& Error );
	UXC_TcpipConnection* GetServerConnection();
};

