/*=============================================================================
	HTTP.cpp
	Author: Fernando Velazquez

	Implementation of asynchronous HTTP File Downloader
=============================================================================*/

#if 1

#include "XC_IpDrv.h"
#include "HTTPDownload.h"
#include "Cacus/CacusBase.h"
#include "Cacus/DynamicLinking.h"
#include "Cacus/Atomics.h"
#include "Cacus/TCharBuffer.h"

/*----------------------------------------------------------------------------
	LibCurl utils.
----------------------------------------------------------------------------*/

typedef int32 (*curl_global_init_PROC)(int32);
typedef void* (*curl_easy_init_PROC)(void);
typedef void (*curl_easy_cleanup_PROC)(void*);
typedef int32 (*curl_easy_setopt_PROC)(void*, int32, ...);
typedef int32 (*curl_easy_perform_PROC)(void*);
typedef const char* (*curl_easy_strerror_PROC)(int32);
typedef int32 (*curl_easy_getinfo_PROC)(void*,int32,...);

static UXC_HTTPDownload* CallbackDownload = nullptr;
static void* CURLEasy = nullptr;
static int32 ContentLengthQuery = 0;
static curl_global_init_PROC curl_global_init;
static curl_easy_init_PROC curl_easy_init;
static curl_easy_cleanup_PROC curl_easy_cleanup;
static curl_easy_setopt_PROC curl_easy_setopt;
static curl_easy_perform_PROC curl_easy_perform;
static curl_easy_strerror_PROC curl_easy_strerror;
static curl_easy_getinfo_PROC curl_easy_getinfo;

static UBOOL LibCurl_GetProcs( CScopedLibrary* LibHandle)
{
	curl_global_init   = LibHandle->Get<curl_global_init_PROC>("curl_global_init");
	curl_easy_init     = LibHandle->Get<curl_easy_init_PROC>("curl_easy_init");
	curl_easy_cleanup  = LibHandle->Get<curl_easy_cleanup_PROC>("curl_easy_cleanup");
	curl_easy_setopt   = LibHandle->Get<curl_easy_setopt_PROC>("curl_easy_setopt");
	curl_easy_perform  = LibHandle->Get<curl_easy_perform_PROC>("curl_easy_perform");
	curl_easy_strerror = LibHandle->Get<curl_easy_strerror_PROC>("curl_easy_strerror");
	curl_easy_getinfo  = LibHandle->Get<curl_easy_getinfo_PROC>("curl_easy_getinfo");
	return curl_global_init 
		&& curl_easy_init 
		&& curl_easy_cleanup 
		&& curl_easy_setopt 
		&& curl_easy_perform 
		&& curl_easy_strerror
		&& curl_easy_getinfo;
}

static size_t TestCallback( void* Data, size_t Size, size_t Elems, void* Arg)
{
	int32 Received = (int32)(Size * Elems);
	if ( CallbackDownload )
	{
		if ( ContentLengthQuery )
		{
			/*CURLINFO_CONTENT_LENGTH_DOWNLOAD_T*/
			int_p ContentLength;
			if ( !curl_easy_getinfo( CURLEasy, 0x600000 + 15, &ContentLength) )
			{
				CallbackDownload->RealFileSize = (int32)ContentLength;
				ContentLengthQuery = 0;
			}
		}
		int32 RealSize = CallbackDownload->RealFileSize ? CallbackDownload->RealFileSize : CallbackDownload->Info->FileSize;
		int32 Count = (CallbackDownload->Transfered + Received > RealSize) ? RealSize - CallbackDownload->Transfered : Received;
		if ( Count > 0 )
			CallbackDownload->ReceiveData( (uint8*)Data, Count);
	}
	return (size_t)Received;
}

static size_t HeaderCallback( void* Data, size_t Size, size_t Elems, void* Arg)
{
	size_t Received = Size * Elems;
	/*if ( CallbackDownload )
	{
		TCharWideBuffer<2048> Buffer = (char*)Data;
		CallbackDownload->SavedLogs.Logf( NAME_DevNet, *Buffer);
	}*/
	return Received;
}

/*----------------------------------------------------------------------------
	HTTP Request utils.
----------------------------------------------------------------------------*/

FString HTTP_Request::String()
{
	FString Result = FString::Printf( TEXT("%s %s HTTP/1.1\r\nHost: %s\r\n")
		,*Method, *Path, *Hostname);
	for ( TMultiMap<FString,FString>::TIterator It(Headers) ; (UBOOL)It ; ++It )
		Result += FString::Printf( TEXT("%s: %s\r\n"), *It.Key(), *It.Value() );
	Result += TEXT("\r\n");
	return Result;
}

/*----------------------------------------------------------------------------
	HTTP Downloader.
----------------------------------------------------------------------------*/

void UXC_HTTPDownload::StaticConstructor()
{
	UClass* Class = GetClass();

	// Config.
	new(Class,TEXT("ProxyServerHost"),		RF_Public)UStrProperty(CPP_PROPERTY(ProxyServerHost		), TEXT("Settings"), CPF_Config );
	new(Class,TEXT("ProxyServerPort"),		RF_Public)UIntProperty(CPP_PROPERTY(ProxyServerPort		), TEXT("Settings"), CPF_Config );
	new(Class,TEXT("DownloadTimeout"),		RF_Public)UFloatProperty(CPP_PROPERTY(DownloadTimeout	), TEXT("Settings"), CPF_Config );
	new(Class,TEXT("RedirectToURL"),		RF_Public)UStrProperty(CPP_PROPERTY(DownloadParams		), TEXT("Settings"), CPF_Config );
	new(Class,TEXT("UseCompression"),		RF_Public)UBoolProperty(CPP_PROPERTY(UseCompression		), TEXT("Settings"), CPF_Config );

	//Defaults
	UXC_HTTPDownload* Defaults = (UXC_HTTPDownload*)Class->GetDefaultObject();
	Defaults->IsLZMA = 1;
	Defaults->IsCompressed = 1;
	Defaults->DownloadTimeout = 4.0f;
}

UXC_HTTPDownload::UXC_HTTPDownload()
{
	Socket.SetInvalid();
	IsCompressed = 0;
	IsLZMA = 0;
}

void UXC_HTTPDownload::Destroy()
{
	if ( CURL_Library )
	{
		delete CURL_Library;
		CURL_Library = nullptr;
	}
	Socket.Close();
	Super::Destroy();
}

UBOOL UXC_HTTPDownload::TrySkipFile()
{
	if( Super::TrySkipFile() )
	{
		Connection->Logf( TEXT("SKIP GUID=%s"), *Info->Guid.String() );
		Socket.Close();
		Finished = 1;
		Error[0] = '\0';
		return 1;
	}
	return 0;
}

void UXC_HTTPDownload::ReceiveFile( UNetConnection* InConnection, INT InPackageIndex, const TCHAR* Params, UBOOL InCompression)
{
	guard(UXC_HTTPDownload::ReceiveFile);
	SaveConfig();
	UDownload::ReceiveFile( InConnection, InPackageIndex);

	if( !Params[0] )
	{
		IsInvalid = 1;
		Finished = 1;
		return;
	}

	FPackageInfo& Info = Connection->PackageMap->List( PackageIndex );
	DownloadURL = FDownloadURL( Params, *Info.URL);
	if ( !DownloadURL.bIsValid )
	{
		IsInvalid = 1;
		Finished = 1;
		return;
	}

	if ( InCompression )
		DownloadURL.Compression = LZMA_COMPRESSION;
	DownloadURL.ProxyHostname = ProxyServerHost;
	DownloadURL.ProxyPort = ProxyServerPort;
	CurrentURL = DownloadURL;

	if ( DownloadURL.Scheme == TEXT("http") )
	{
		Request = HTTP_Request();
		Request.Hostname = CurrentURL.StringHost();
		Request.Method = TEXT("GET");
		Request.Path = CurrentURL.StringGet();
		Request.Headers.Set( TEXT("User-Agent")  , TEXT("Unreal"));
		Request.Headers.Set( TEXT("Accept")      , TEXT("*/*"));
		Request.Headers.Set( TEXT("Connection")  , TEXT("close")); //Proxy with auth require [Proxy-Connection: close]
		Request.RedirectsLeft = 5;
	}
	else if ( DownloadURL.Scheme == TEXT("https") )
	{
		CURL_Library = new CScopedLibrary( "libcurl" CACUSLIB_LIBRARY_EXTENSION);
		if ( !CURL_Library->Handle )
		{
			IsInvalid = 1;
			Finished = 1;
			return;
		}
	}
	else
	{
		IsInvalid = 1;
		Finished = 1;
		return;
	}
	FString Msg1 = FString::Printf( (Info.PackageFlags&PKG_ClientOptional)?LocalizeProgress(TEXT("ReceiveOptionalFile"),TEXT("Engine")):LocalizeProgress(TEXT("ReceiveFile"),TEXT("Engine")), Info.Parent->GetName() );
	Connection->Driver->Notify->NotifyProgress( *Msg1, TEXT(""), 4.f );

	unguard;
}




void UXC_HTTPDownload::Tick()
{
	SavedLogs.Flush();
	Super::Tick();

	/* HTTPS */
	if ( !Finished && !AsyncAction && (CurrentURL.Scheme == TEXT("https")) && CURL_Library && CURL_Library->Handle )
	{
		Request.Hostname = CurrentURL.String();
		debugf( NAME_DevNet, TEXT("Requesting %s..."), *Request.Hostname);

		//***********
		// Setup CURL
		new FDownloadAsyncProcessor( [](FDownloadAsyncProcessor* Proc)
		{
			//STAGE 1, setup local environment.
			FString Error;
			UXC_HTTPDownload* Download = (UXC_HTTPDownload*)Proc->Download;

			if ( LibCurl_GetProcs(Download->CURL_Library) )
			{
				//STAGE 2, let main go (no longer safe to use Download from now on)
				Proc->Detach();
				int32 GlobalInitCode = curl_global_init(0b11);
				if ( !GlobalInitCode )
				{
					CURLEasy = curl_easy_init();
					if ( CURLEasy )
					{
						TChar8Buffer<1024> RequestURL = *Download->Request.Hostname;
						curl_easy_setopt( CURLEasy, 10000 +  2 /*URL*/, *RequestURL);
						curl_easy_setopt( CURLEasy, 00000 + 64 /*SSL_VERIFYPEER*/, 0);
						curl_easy_setopt( CURLEasy, 00000 + 81 /*SSL_VERIFYHOST*/, 0);
						curl_easy_setopt( CURLEasy, 00000 + 13,/*TIMEOUT*/ appCeil(Download->DownloadTimeout));
						curl_easy_setopt( CURLEasy, 00000 + 45,/*FAILONERROR*/ 1); 
						curl_easy_setopt( CURLEasy, 00000 + 41,/*VERBOSE*/ 1);
						curl_easy_setopt( CURLEasy, 20000 + 11,/*WRITEFUNCTION*/ TestCallback);
						curl_easy_setopt( CURLEasy, 20000 + 79,/*HEADERFUNCTION*/ HeaderCallback);

						{
							//STAGE 3, validate downloader and lock
							CSpinLock SL(&UXC_Download::GlobalLock);
							if ( Proc->DownloadActive() )
							{
								CallbackDownload = Download;
								ContentLengthQuery = 1;
								int32 DownloadResult = curl_easy_perform(CURLEasy);
//								Download->SavedLogs.Logf( *FString::Printf(TEXT("LibCurl status %i (%s)"), DownloadResult, appFromAnsi(curl_easy_strerror(DownloadResult))) );
								if ( DownloadResult )
								{
									// Timed out, this redirect is unreachable
									if ( DownloadResult == 28 ) 
									{
										Download->IsInvalid = 1;
										Error = UXC_Download::ConnectionFailedError;
									}
									// If LZMA fails, try UZ, then no compression.
									else if ( Download->CurrentURL.Compression > 0 )
										Download->CurrentURL.Compression--;
									// All methods failed, this redirect doesn't have this file.
									else
										Error = FString::Printf( *UXC_Download::InvalidUrlError, *Download->CurrentURL.String());
								}

								if ( Download->RecvFileAr )
								{
									delete Download->RecvFileAr;
									Download->RecvFileAr = nullptr;
								}

								CallbackDownload = nullptr;
							}
						}
						curl_easy_cleanup(CURLEasy);
						CURLEasy = nullptr;
					}
					else Error = TEXT("CURL easy init error");
				}
				else
				{
					TCharWideBuffer<256> Reason = curl_easy_strerror(GlobalInitCode); // Convert to UNICODE
					Error = FString::Printf( TEXT("CURL error: %s"), *Reason);
				}
			}
			else Error = TEXT("Unable to retrieve libcurl entry points.");

		if ( Proc->DownloadActive() && Error.Len() )
			Download->DownloadError(*Error);

		}, this);

		return;
	}


	//*****************************
	// Async operations have 3 stages:
	// 1 -- Setup stage:
	//  Initialization of the async process, this blocks the main thread.
	// 2 -- Async stage:
	//  The async processor runs downloader independent tasks (mostly API stuff) while the
	//  main thead continues as normal, polling the downloader for updates on every tick.
	// 3 -- End stage:
	//  The async processor blocks destruction of downloaders and begins to update them.
	if ( !Finished && !AsyncAction )
	{
		//******************************
		//Setup hostname and update port
		FString NewHostname = CurrentURL.StringHost( 1 );
		if ( Request.Hostname != NewHostname )
		{
			RemoteEndpoint.Address = IPAddress::Any;
			Request.Hostname = NewHostname;
		}
		RemoteEndpoint.Port = CurrentURL.Port ? CurrentURL.Port : 80;

		//*****************************
		//Hostname needs to be resolved
		if ( RemoteEndpoint.Address == IPAddress::Any )
		{
			new FDownloadAsyncProcessor( [](FDownloadAsyncProcessor* Proc)
			{
				//STAGE 1, setup local environment.
				UXC_HTTPDownload* Download = (UXC_HTTPDownload*)Proc->Download;
				FString Hostname = Download->Request.Hostname;

				//STAGE 2, let main go (no longer safe to use Download from now on)
				Proc->Detach();
				IPAddress Address = CSocket::ResolveHostname( appToAnsi(*Hostname));

				//STAGE 3, validate downloader and lock
				CSpinLock SL(&UXC_Download::GlobalLock);
				if ( !Proc->DownloadActive() )
					return;
				if ( Address == IPAddress::Any )
				{
					Download->SavedLogs.Logf( NAME_DevNet, TEXT("Failed to resolve hostname %s"), *Hostname);
					Download->DownloadError( *FString::Printf( *UXC_Download::InvalidUrlError, *Download->Request.Hostname) );
				}
				else
					Download->SavedLogs.Logf( NAME_DevNet, TEXT("Resolved: %s >> %s"), *Hostname, appFromAnsi(*Address) );
				Download->RemoteEndpoint.Address = Address;
			}, this);
		}

		//************
		//Send request
		else if ( Request.Path.Len() )
		{
			Response = HTTP_Response();
			Request.Path = CurrentURL.StringGet();
			new FDownloadAsyncProcessor( [](FDownloadAsyncProcessor* Proc)
			{
				//STAGE 1, setup local environment.
				UXC_HTTPDownload* Download = (UXC_HTTPDownload*)Proc->Download;
				if ( !Download->AsyncLocalBind() )
					return;
				CSocket Socket               = Download->Socket;
				double Timeout               = Max<double>( Download->DownloadTimeout, 2.0);
				IPEndpoint RemoteEndpoint    = Download->RemoteEndpoint;
				FString RequestHeader        = Download->Request.String();
				TCharWideBuffer<256> ConnectError;

				//STAGE 2, let main go (no longer safe to use Download from now on)
				Proc->Detach();
				Socket.SetNonBlocking();
				appSleep( 0.2f); //Don't try to connect so quickly (a previous download's connection may not be closed)
				ESocketState State = SOCKET_MAX;
				if ( !Socket.Connect(RemoteEndpoint) && !Socket.IsNonBlocking(Socket.LastError) )
				{
					TCharWideBuffer<64> ErrorCode = Socket.ErrorText(Socket.LastError);
					ConnectError = TEXT("XC_HTTPDownload: connect() failed ");
					ConnectError += *ErrorCode;
				}	
				else
				{
					State = Socket.CheckState( SOCKET_Writable, Timeout);
					if ( State == SOCKET_HasError )
						ConnectError = TEXT("XC_HTTPDownload: select() failed");
					else if ( State == SOCKET_Timeout )
						ConnectError = TEXT("XC_HTTPDownload: connection timed out");
				}

				if ( ConnectError[0] != '\0')
				{
					//STAGE 3: Tell downloader (if still exists) of failure to connect to server
					CSleepLock SL(&UXC_Download::GlobalLock); 
					if ( Proc->DownloadActive() )
					{
						Download->SavedLogs.Log( NAME_DevNet, *ConnectError);
						appSleep( 0.1f);
						Download->DownloadError( *UXC_Download::ConnectionFailedError );
						Download->IsInvalid = (State == SOCKET_Timeout); //If the server timed out, do not try to connect again for another download
					}
					return;
				}
				else
				{
					//STAGE 3: Send request to server and unlock again (back to stage 2)
					CSleepLock SL(&UXC_Download::GlobalLock); 
					if ( !Proc->DownloadActive() )
						return;

					int32 Sent = 0;
					const ANSICHAR* RequestHeaderAnsi = appToAnsi( *RequestHeader);
					bool bSent = Socket.Send( (const uint8*)RequestHeaderAnsi, RequestHeader.Len(), Sent) && (Sent >= RequestHeader.Len());
					if ( !bSent ) //Produce proper log!
					{
						ConnectError = TEXT("XC_HTTPDownload: send() failed with ");
						TCharWideBuffer<64> ErrorCode = Socket.ErrorText(Socket.LastError);
						ConnectError += *ErrorCode;
						Download->SavedLogs.Log( NAME_DevNet, *ConnectError);
						appSleep( 0.1f);
						Download->DownloadError( *UXC_Download::ConnectionFailedError );
						return;
					}
					Download->SavedLogs.Log( NAME_DevNetTraffic, TEXT("Connected..."));
				}

				double LastRecvTime = appSecondsNew();
				while ( true )
				{
					//STAGE 3: Receive data from server
					CSleepLock SL(&UXC_Download::GlobalLock); 
					if ( !Proc->DownloadActive() )
						return;
					int32 OldTransfered = Download->Transfered;
					if ( Download->AsyncReceive() )
					{
						if ( Download->Error[0] )
							Download->SavedLogs.Log( Download->Error);
						break;
					}
					if ( OldTransfered != Download->Transfered )
						LastRecvTime = appSecondsNew();
					else if ( appSecondsNew() - LastRecvTime > Timeout )
					{
						Download->SavedLogs.Log( NAME_DevNet, TEXT("XC_HTTPDownload: connection timed out") );
						Download->DownloadError( *UXC_Download::ConnectionFailedError );
						break;
					}
					appSleep( 0); //Poll aggresively to prevent bandwidth loss
				}
				CSleepLock SL(&UXC_Download::GlobalLock); 
				if ( Proc->DownloadActive() && Download->RecvFileAr )
				{
					delete Download->RecvFileAr;
					Download->RecvFileAr = nullptr;
				}
			}, this);
		}
	}
}

/*----------------------------------------------------------------------------
	Downloader utils.
----------------------------------------------------------------------------*/

void UXC_HTTPDownload::UpdateCurrentURL( const TCHAR* RelativeURI)
{
	//Merge modifiers into path
	if ( CurrentURL.RequestedPackage.Len() )
	{
		CurrentURL.Path += CurrentURL.RequestedPackage;
		CurrentURL.Path += CurrentURL.GetCompressedExt( CurrentURL.Compression);
		CurrentURL.RequestedPackage.Empty();
		CurrentURL.Compression = 0;
	}
	*(FURI*)&CurrentURL = FURI( CurrentURL, RelativeURI);
}

/*----------------------------------------------------------------------------
	Asynchronous functions.
	These are called by worker threads.
----------------------------------------------------------------------------*/

void UXC_HTTPDownload::ReceiveData( BYTE* Data, INT Count )
{
	if ( Count <= 0 )
		return;

	// Receiving spooled file data.
	if( !Transfered && !RecvFileAr )
	{
		// Open temporary file initially.
		SavedLogs.Logf( NAME_DevNet, TEXT("Receiving package '%s'"), Info->Parent->GetName() );
		if ( RealFileSize )
			SavedLogs.Logf( NAME_DevNet, TEXT("Compressed filesize: %i"), RealFileSize);

		GFileManager->MakeDirectory( *GSys->CachePath, 0 );
		GFileManager->MakeDirectory( TEXT("../DownloadTemp"), 0);
		FString Filename = FString::Printf( TEXT("../DownloadTemp/%s%s"), *DownloadURL.RequestedPackage, DownloadURL.GetCompressedExt(DownloadURL.Compression) );
		appStrncpy( TempFilename, *Filename, 255);
		RecvFileAr = GFileManager->CreateFileWriter( TempFilename );
		if ( Count >= 13 )
		{
			QWORD* LZMASize = (QWORD*)&Data[5];
			if ( Info->FileSize == *LZMASize )
			{
				IsCompressed = 1;
				IsLZMA = 1;
				SavedLogs.Logf( NAME_DevNet, TEXT("USES LZMA"));
			}
			INT* UzSignature = (INT*)&Data[0];
			if ( *UzSignature == 1234 || *UzSignature == 5678 )
			{
				IsCompressed = 1;
				SavedLogs.Logf( NAME_DevNet, TEXT("USES UZ: Signature %i"), *UzSignature);
			}
		}
	}

	// Receive.
	if( !RecvFileAr )
		DownloadError( *UXC_Download::NetOpenError );
	else
	{
		RecvFileAr->Serialize( Data, Count);
		if( RecvFileAr->IsError() )
			DownloadError( *FString::Printf( *UXC_Download::NetWriteError, TempFilename ) );
		else
			Transfered += Count;
	}	
}


bool UXC_HTTPDownload::AsyncReceive()
{
	uint8 Buf[4096];
	int32 Bytes = 0;
	int32 TotalBytes = 0;
	bool bShutdown = false;
	while ( Socket.Recv( Buf, sizeof(Buf), Bytes) )
	{
		if ( Bytes == 0 )
		{
			bShutdown = true;
			SavedLogs.Logf( NAME_DevNetTraffic, TEXT("Graceful shutdown"));
			break;
		}
		TotalBytes += Bytes;
		int32 Start = Response.ReceivedData.Add(Bytes);
		appMemcpy( &Response.ReceivedData(Start), Buf, Bytes);
		SavedLogs.Logf( NAME_DevNetTraffic, TEXT("Received %i bytes"), Bytes);
	}

	if ( (Socket.LastError != 0) && !Socket.IsNonBlocking(Socket.LastError) )
	{
		DownloadError( *FString::Printf( TEXT("Socket error: %s"), appFromAnsi(CSocket::ErrorText(Socket.LastError))) );
		return true;
	}

	//Process received bytes
	if ( Response.Status == 0 ) //Header stage
	{
		for ( int32 i=0 ; i<Response.ReceivedData.Num()-1 ; i++ )
			if ( (Response.ReceivedData(i) == '\r') && (Response.ReceivedData(i+1) == '\n') )
			{
				Response.HeaderLines.AddZeroed();
				if ( i == 0 ) //Empty line: EOH
				{
					SavedLogs.Logf( NAME_DevNetTraffic, TEXT("HTTP Header END received") );
					Response.ReceivedData.Remove( 0, 2);
					break;
				}
				TArray<TCHAR>& NewLine = Response.HeaderLines.Last().GetCharArray();
				NewLine.Add( i+1);
				for ( int32 j=0 ; j<i ; j++ )
					NewLine(j) = (TCHAR)Response.ReceivedData(j);
				NewLine(i) = '\0';
				SavedLogs.Logf( NAME_DevNetTraffic, TEXT("HTTP Header received: %s"), *Response.HeaderLines.Last() );

				//Remove raw line and keep processing
				Response.ReceivedData.Remove( 0, i+2);
				i = -1;
			}

		//EOH included
		if ( Response.HeaderLines.Num() && (Response.HeaderLines.Last().Len() == 0) )
		{
			if ( Response.HeaderLines.Num() < 2 )
			{
				Response.Status = -1; //Internal error
				DownloadError( TEXT("Bad HTTP response") );
				return true;
			}
			const TCHAR* Line = *Response.HeaderLines(0);
			Response.Version = ParseToken( Line, 0);
			Response.Status = appAtoi( *ParseToken( Line, 0) );
			for ( int32 i=1 ; i<Response.HeaderLines.Num()-1 ; i++ )
			{
				Line = *Response.HeaderLines(i);
				FString Key = ParseToken( Line, 0);
				if ( Key[Key.Len()-1] == ':' )
					Key.GetCharArray().Remove( Key.Len()-1 );
				while ( *Line == ' ' )
					Line++;
				Response.Headers.Set( *Key, Line);
			}
			Response.HeaderLines.Empty();
		}
		else
			return bShutdown;

		AGAIN:
		if ( Response.Status == 200 ) //OK
		{
			//Cookie, only one supported for now
			FString* SetCookie = Response.Headers.Find( TEXT("Set-Cookie"));
			if ( SetCookie )
				Request.Headers.Set( TEXT("Cookie"), **SetCookie);
			//Get filesize
			FString* ContentLength = Response.Headers.Find( TEXT("Content-Length"));
			if ( ContentLength )
			{
				RealFileSize = appAtoi( **ContentLength);
				if ( RealFileSize == 0 )
				{
					Response.Status = 404;
					goto AGAIN;
				}
			}
		}
		else if ( Response.Status == 301 || Response.Status == 302 || Response.Status == 303 ||  Response.Status == 307 ) //Permanent redirect + Found + Temporary redirect
		{
			if ( Response.Status == 303 )
				Request.Method = TEXT("GET");
			FString* Location = Response.Headers.Find( TEXT("Location"));
			if ( Location )
				SavedLogs.Logf( NAME_DevNet, TEXT("Redirected (%i) to %s"), Response.Status, **Location);
			if ( !Location || !Location->Len() )
				DownloadError( TEXT("Bad redirection"));
			else if (Request.RedirectsLeft-- <= 0 )
				DownloadError( TEXT("Too many redirections") );
			else
				UpdateCurrentURL( **Location);
			return true;
		}
		else if ( Response.Status == 404 )
		{
			if ( DownloadURL.Compression > 0 ) //Change compression and retry
			{
				DownloadURL.Compression--;
				CurrentURL = DownloadURL;
			}
			else
				DownloadError( *FString::Printf( *UXC_Download::InvalidUrlError, *CurrentURL.String() ) );
			return true;
		}
		else
		{
			DownloadError( *FString::Printf( *UXC_Download::InvalidUrlError, *CurrentURL.String() ) );
			return true;
		}
	}
	
	if ( Response.Status == 200 )
	{
		int32 RealSize = RealFileSize ? RealFileSize : Info->FileSize;
		int32 Count = (Transfered + Response.ReceivedData.Num() > RealSize) ? RealSize - Transfered : Response.ReceivedData.Num();
		if ( Count > 0 )
			ReceiveData( &Response.ReceivedData(0), Count );
		Response.ReceivedData.Empty();
		if ( ((Transfered >= RealSize) || bShutdown) && RecvFileAr )
		{
			delete RecvFileAr;
			RecvFileAr = nullptr;
			bShutdown = true;
		}
	}

	return bShutdown;
}

bool UXC_HTTPDownload::AsyncLocalBind()
{
	Socket.Close();
	Socket = CSocket(true);
	if ( Socket.IsInvalid() )
	{
		DownloadError( *UXC_Download::ConnectionFailedError );
		return false;
	}
	Socket.SetReuseAddr();
	Socket.SetLinger();
	TArray<IPAddress> LocalAddresses = GetLocalBindAddress(SavedLogs);
	for ( int32 i=0; i<LocalAddresses.Num(); i++)
	{
		IPEndpoint LocalEndpoint( LocalAddresses(i), 0);
		if ( Socket.BindPort(LocalEndpoint, 20) )
			return true;
	}
	SavedLogs.Log( NAME_DevNet, TEXT("XC_HTTPDownload: bind() failed") );
	DownloadError( *UXC_Download::ConnectionFailedError );
	return false;
}

IMPLEMENT_CLASS(UXC_HTTPDownload)

#endif