/*
 * Licensed materials, Property of IBM Corp.
 *
 * openCryptoki CCA token
 *
 * (C) COPYRIGHT International Business Machines Corp. 2001, 2002, 2006
 *
 * Author: Kent E. Yoder <yoder1@us.ibm.com>
 *
 */
 
/***************************************************************************
                          Change Log
                          ==========
       4/25/03    Kapil Sood (kapil@corrent.com)
                  Added DH key pair generation and DH shared key derivation
                  functions.
 
 
 
****************************************************************************/

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <strings.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <grp.h>
#include <pwd.h>

#include "cca_stdll.h"

#include "pkcs11types.h"
#include "stdll.h"

#include "defs.h"
#include "host_defs.h"
#include "h_extern.h"
#include "tok_spec_struct.h"
#include "pkcs32.h"

#include "../api/apiproto.h"

#define UCHAR  unsigned char

/* Declared in obj_mgr.c */
extern pthread_rwlock_t obj_list_rw_mutex;

char *pk_dir;
void SC_SetFunctionList(void);

#define SESSION_MGR_FIND(x) session_mgr_find(x) /* All these need to
						 * get the lock */

/* Maximum number of supported devices (rather arbitrary) */
#define PKW_MAX_DEVICES                10

// Netscape/SSL is fairly timing-sensitive so can't always use a debugger
//
// If the CRYPTOKI_DEBUG environment variable is defined, information
// about each successful PKCS#11 call made is written to the file named
// in that environment variable.
//
// If the CRYPTOKI_PROFILE environment variable is defined, information
// about the amount of time spent in each PKCS#11 API during a "run"
//
// If the CRYPTOKI_DEBUG environment variable is defined, information
// about each successful PKCS#11 call made is written to the file named
// in that environment variable.
//
// If the CRYPTOKI_PROFILE environment variable is defined, information
// about the amount of time spent in each PKCS#11 API during a "run"
// (i.e., from _DLL_InitTerm init call to _DLL_InitTerm term call) is
// appended to the file named in that environment variable.
//
// If the CRYPTOKI_STATS_FILE environment variable is defined, information
// about various internal metrics at the end of a "run" is appended to
// the file named in that environment variable.  The CRYPTOKI_STATS
// environment variable specifies the argument(s) that are passed to
// the function that returns the metrics to specify which metric(s) are
// returned.
//
#define MAXFILENAME 1024

static char *debugfilepathbuffer;
static int debugon = 1;
int  debugfile = 0;
#define FFLUSH(x) 

pid_t  initedpid=0;  // for initialized pid

CK_ULONG  usage_count = 0; // variable for number of times the DLL has
			   // been used.

CK_C_INITIALIZE_ARGS cinit_args = { NULL, NULL, NULL, NULL, 0, NULL };

extern void stlogterm();
extern void stloginit();
extern void stlogit2(int type,char *fmt, ...);

CK_BBOOL
st_Initialized()
{
	if (initialized == FALSE ) return FALSE;
	return TRUE;
}

#ifdef SPINXPL
extern int spinxplfd;
extern int spin_created;
#endif

void
Fork_Initializer(void)
{
	stlogterm();
        stloginit(); // Initialize Logging so we can capture
		     // EVERYTHING

#ifdef SPINXPL
	spinxplfd = -1;
	spin_created = 0;
#endif

	// Force logout.  This cleans out the private session and list
	// and cleans out the private object map
	session_mgr_logout_all();

	// Clean out the public object map
	// First parm is no longer used..
	object_mgr_purge_map((SESSION *)0xFFFF, PUBLIC);
	object_mgr_purge_map((SESSION *)0xFFFF, PRIVATE);

	// This should clear the entire session list out
	session_mgr_close_all_sessions();

	//next_session_handle = 1; // Make is so sessions start with 1
	//next_object_handle = 1;

	// Clean out the global login state variable
	// When implemented...  Although logout_all should clear this up.

	// Once the object_map is flushed, the obj_lists (public and
	// private) are both just linked lists that have to be freed
	// up...  //logit("%s:%d: tokenobj publ 0x%08x priv
	// 0x%08x",__FILE__,__LINE__,publ_token_obj_list,
	// priv_token_obj_list);
	while (priv_token_obj_list) {
		priv_token_obj_list = dlist_remove_node(priv_token_obj_list,
							priv_token_obj_list);
	}

	while (publ_token_obj_list) {
		publ_token_obj_list = 
			dlist_remove_node(publ_token_obj_list,
					  publ_token_obj_list);
	}

	// Need to do something to prevent the shared memory from
	// having the objects loaded again.... The most likely place
	// is in the obj_mgr file where the object is added to shared
	// memory (object_mgr_add_to_shm) a query should be done to
	// the appropriate object list....
}

#ifdef ALLLOCK
#define LOCKIT   pthread_mutex_lock(&native_mutex)
#define LLOCK
#define UNLOCKIT   pthread_mutex_unlock(&native_mutex)
#else
#ifdef DEBLOCK
#define LOCKIT
#define LLOCK   pthread_mutex_lock(&native_mutex)
#define UNLOCKIT   pthread_mutex_unlock(&native_mutex)
#else
#define LOCKIT
#define LLOCK
#define UNLOCKIT
#endif
#endif

int
APISlot2Local(snum)
CK_SLOT_ID  snum;
{
	return(token_specific.t_slot2local(snum));
}


#define  SLT_CHECK  \
   CK_SLOT_ID     slot_id; \
   int            sid1; \
 \
   if ( (sid1 = APISlot2Local(sid)) != -1 ){ \
      slot_id = sid1; \
   } else { \
      return CKR_ARGUMENTS_BAD; \
   }


#define SESSION_HANDLE   sSession.sessionh
#define SLOTID    APISlot2Local(sSession.slotID)

#define SESS_SET \
   CK_SESSION_HANDLE  hSession; \
\
   hSession = sSession.sessionh;


// More efficient long reverse
inline CK_ULONG long_reverse(CK_ULONG x)
{
#ifdef _POWER   // Power Architecture requires reversal to talk to adapter
	return (
		((0x000000FF & x)<<24) |
		((0x0000FF00 & x)<<8) |
		((0x00FF0000 & x)>>8) |
		((0xFF000000 & x)>>24) );
#else
	return (x); // Others don't require  reversal.
#endif

}

// verify that the mech specified is in the
// mech list for this token... Common code requires this 
// to be added
CK_RV 
validate_mechanism(CK_MECHANISM_PTR  pMechanism)
{
	CK_ULONG i;   
	for (i=0; i< mech_list_len;i++){
		if (pMechanism->mechanism == mech_list[i].mech_type) {
			return CKR_OK;
		}
	}
	ock_log_err(OCK_E_MECH_INV);
	return CKR_MECHANISM_INVALID;
}

#define VALID_MECH(p) \
   if ( validate_mechanism(p) != CKR_OK){ \
      rc = CKR_MECHANISM_INVALID; \
      goto done; \
   } \

// Defines to allow NT code to work correctly
#define WaitForSingleObject(x,y)  pthread_mutex_lock(&(x))
#define ReleaseMutex(x)           pthread_mutex_unlock(&(x))

void
init_data_store(char *directory)
{
	char *pkdir;
	if ( (pkdir = getenv("PKCS_APP_STORE")) != NULL){
		pk_dir =  (char *) malloc(strlen(pkdir)+1024);
		memset(pk_dir, 0, strlen(pkdir)+1024);
		sprintf(pk_dir,"%s/%s",pkdir,SUB_DIR);
	} else {
		pk_dir  = (char *)malloc(strlen(directory)+25);
		memset(pk_dir, 0, strlen(directory)+25);
		sprintf(pk_dir,"%s",directory);

	}
}

/* In an STDLL this is called once for each card in the system
 * therefore the initialized only flags certain one time things
 * However in the case of a lightened accelerator, the cards are all
 * agregated together in a single token.  Therefore the correlator
 * should be a list of device names which have either the correct clu
 * or the crypt light adapter... */
CK_RV
ST_Initialize(void **FunctionList,
	      CK_SLOT_ID SlotNumber,
	      char *Correlator)
{
	int    i;
	CK_RV  rc = CKR_OK;
	struct passwd  *pw,*epw; // SAB XXX XXX
	uid_t    userid,euserid;

	stlogterm();
	stloginit();

	// Check for root user or Group PKCS#11 Membershp
	// Only these are allowed.
	userid = getuid();
	euserid = geteuid();

	if (userid != 0 && euserid != 0) { // Root or effective Root
					   // is ok
		struct group *grp;
		int   rc = 0;
		gid_t  gid,egid;
		grp = getgrnam("pkcs11");
		if (grp) {
			// Check for member of group..
			// SAB get login seems to not work with some
			// instances of application invocations
			// (particularly when forked).  So we need to
			// get the group informatiion.  Really need to
			// take the uid and map it to a name.
			pw = getpwuid(userid);
			epw = getpwuid(euserid);
			gid = getgid();
			egid = getegid();
         
			if ( gid == grp->gr_gid || egid == grp->gr_gid){
				rc = 1;
			} else {
				i = 0;
				while (grp->gr_mem[i]) {
					if (pw) {
						if (strncmp(pw->pw_name, 
							    grp->gr_mem[i],
							    strlen(pw->pw_name))
						    == 0 ) {
							rc = 1;
							break;
						}
					}
					if (epw) {
						if (strncmp(epw->pw_name,
							    grp->gr_mem[i],
							    strlen(epw->pw_name))
						    == 0 ){
							rc = 1;
							break;
						}
					}
					i++;
				}
			}
			if (rc == 0 ){
				ock_log_err(OCK_E_FUNC);
				return CKR_FUNCTION_FAILED;
			}
		} else {
			ock_log_err(OCK_E_FUNC);
			return CKR_FUNCTION_FAILED;
		}
	}

	// assume that the upper API prevents multiple calls of initialize
	// since that only happens on C_Initialize and that is the
	// resonsibility of the upper layer..
	initialized = FALSE; /// So the rest of the code works correctly

	// If we're not already initialized, grab the mutex and do the
	// initialization.  Check to see if another thread did so while we
	// were waiting...
	//
	// One of the things we do during initialization is create the mutex for
	// PKCS#11 operations; until we do so, we have to use the native mutex...
	//
	WaitForSingleObject( native_mutex, INFINITE );

	// SAB need to call Fork_Initializer here
	// instead of at the end of the loop...
	// it may also need to call destroy of the following 3 mutexes..
	// it may not matter...
	Fork_Initializer();


	MY_CreateMutex( &pkcs_mutex      );
	MY_CreateMutex( &obj_list_mutex  );
	if (pthread_rwlock_init(&obj_list_rw_mutex, NULL)) {
		st_err_log(145, __FILE__, __LINE__);
	}
	MY_CreateMutex( &sess_list_mutex );
	MY_CreateMutex( &login_mutex     );


	if ( (debugfilepathbuffer = getenv( "CRYPTOKI_DEBUG")) != NULL) {
		debugon=1;
	}

	init_data_store((char *)PK_DIR);


	// Handle global initialization issues first if we have not
	// been initialized.
	if (st_Initialized() == FALSE){
#if SYSVSEM
		xproclock = (void *)&xprocsemid;
		CreateXProcLock(xproclock);
#endif
		if ( (rc = attach_shm()) != CKR_OK) {
			st_err_log(144, __FILE__, __LINE__);
			goto done;
		}
      

		nv_token_data = &global_shm->nv_token_data;

		stloginit();

		initialized = TRUE;
		initedpid = getpid();
		SC_SetFunctionList();

		// Always call the token_specific_init function....
		rc =  token_specific.t_init(Correlator,SlotNumber);
		if (rc != 0) {   // Zero means success, right?!?
			*FunctionList = NULL;
			st_err_log(145, __FILE__, __LINE__);
			goto done;
		}
	}

	rc = load_token_data();
	if (rc != CKR_OK) {
		*FunctionList = NULL;
		st_err_log(145, __FILE__, __LINE__);
		goto done;
	}

	/* no need to return error here, that would only prevent the stdll from loading. We load
	 * the token objects that we can and syslog the rest */
	load_public_token_objects();

	XProcLock( xproclock );
	global_shm->publ_loaded = TRUE;
	XProcUnLock( xproclock );

	init_slotInfo();

	usage_count++;
	(*FunctionList) = &function_list;

 done:
	ReleaseMutex( native_mutex );
	if (rc != 0)
		st_err_log(145, __FILE__, __LINE__);
	return rc;
}

// What does this really have to do in this new token...  probably
// need to close the adapters that are opened, and clear the other
// stuff
CK_RV SC_Finalize( CK_SLOT_ID sid )
{
	CK_RV          rc;
	SLT_CHECK;
	if (st_Initialized() == FALSE) {	
		ock_log_err(OCK_E_API_NOT_INIT);
		return CKR_CRYPTOKI_NOT_INITIALIZED;
	}
	rc = MY_LockMutex( &pkcs_mutex );
	if (rc != CKR_OK){
		st_err_log(146, __FILE__, __LINE__);
		return rc;
	} 
	// If somebody else has taken care of things, leave...
	if (st_Initialized() == FALSE) {
		MY_UnlockMutex( &pkcs_mutex ); // ? Somebody else has
					       // also destroyed the
					       // mutex...
		ock_log_err(OCK_E_API_NOT_INIT);
		return CKR_CRYPTOKI_NOT_INITIALIZED;
	}
	usage_count--;
	if (usage_count == 0){
		initialized = FALSE;
	}
	session_mgr_close_all_sessions();
	object_mgr_purge_token_objects();
	detach_shm();
	// close spin lock file
	if (spin_created)
	  close(spinxplfd);
	if ( token_specific.t_final != NULL) {
		token_specific.t_final();
	}
	rc = MY_UnlockMutex( &pkcs_mutex );
	if (rc != CKR_OK){
		st_err_log(147, __FILE__, __LINE__);
		return rc;
	}
	return CKR_OK;
}

void
copy_token_contents_sensibly(CK_TOKEN_INFO_PTR pInfo, TOKEN_DATA *nv_token_data)
{
	memcpy(pInfo, &nv_token_data->token_info, sizeof(CK_TOKEN_INFO_32));
	pInfo->flags = nv_token_data->token_info.flags;

	if ( nv_token_data->token_info.ulMaxSessionCount == (CK_ULONG_32)CK_UNAVAILABLE_INFORMATION ) {
	  pInfo->ulMaxSessionCount = (CK_ULONG)CK_UNAVAILABLE_INFORMATION;
	} else {
	  pInfo->ulMaxSessionCount = nv_token_data->token_info.ulMaxSessionCount;
	}
	if ( nv_token_data->token_info.ulSessionCount == (CK_ULONG_32)CK_UNAVAILABLE_INFORMATION ) {
	  pInfo->ulSessionCount = (CK_ULONG)CK_UNAVAILABLE_INFORMATION;
	} else {
	  pInfo->ulSessionCount = nv_token_data->token_info.ulSessionCount;
	}
	if ( nv_token_data->token_info.ulMaxRwSessionCount == (CK_ULONG_32)CK_UNAVAILABLE_INFORMATION ) {
	  pInfo->ulMaxRwSessionCount = (CK_ULONG)CK_UNAVAILABLE_INFORMATION;
	} else {
	  pInfo->ulMaxRwSessionCount = 	nv_token_data->token_info.ulMaxRwSessionCount;
	}
	if ( nv_token_data->token_info.ulRwSessionCount == (CK_ULONG_32)CK_UNAVAILABLE_INFORMATION ) {
	  pInfo->ulRwSessionCount = (CK_ULONG)CK_UNAVAILABLE_INFORMATION;
	} else {
	  pInfo->ulRwSessionCount = nv_token_data->token_info.ulRwSessionCount;
	}

	pInfo->ulMaxPinLen = nv_token_data->token_info.ulMaxPinLen;
	pInfo->ulMinPinLen = nv_token_data->token_info.ulMinPinLen;

	if ( nv_token_data->token_info.ulTotalPublicMemory == (CK_ULONG_32)CK_UNAVAILABLE_INFORMATION ) {
	  pInfo->ulTotalPublicMemory = (CK_ULONG)CK_UNAVAILABLE_INFORMATION;
	} else {
	  pInfo->ulTotalPublicMemory = nv_token_data->token_info.ulTotalPublicMemory;
	}
	if ( nv_token_data->token_info.ulFreePublicMemory == (CK_ULONG_32)CK_UNAVAILABLE_INFORMATION ) {
	  pInfo->ulFreePublicMemory = (CK_ULONG)CK_UNAVAILABLE_INFORMATION;
	} else {
	  pInfo->ulFreePublicMemory = nv_token_data->token_info.ulFreePublicMemory;
	}
	if ( nv_token_data->token_info.ulTotalPrivateMemory == (CK_ULONG_32)CK_UNAVAILABLE_INFORMATION ) {
	  pInfo->ulTotalPrivateMemory = (CK_ULONG)CK_UNAVAILABLE_INFORMATION;
	} else {
	  pInfo->ulTotalPrivateMemory = nv_token_data->token_info.ulTotalPrivateMemory;
	}
	if ( nv_token_data->token_info.ulFreePrivateMemory == (CK_ULONG_32)CK_UNAVAILABLE_INFORMATION ) {
	  pInfo->ulFreePrivateMemory = (CK_ULONG)CK_UNAVAILABLE_INFORMATION;
	} else {
	  pInfo->ulFreePrivateMemory = nv_token_data->token_info.ulFreePrivateMemory;
	}

	pInfo->hardwareVersion = nv_token_data->token_info.hardwareVersion;
	pInfo->firmwareVersion = nv_token_data->token_info.firmwareVersion;
	pInfo->flags = long_reverse(pInfo->flags);
	pInfo->ulMaxSessionCount = long_reverse(pInfo->ulMaxSessionCount);
	pInfo->ulSessionCount = long_reverse(pInfo->ulSessionCount);
	pInfo->ulMaxRwSessionCount = long_reverse(pInfo->ulMaxRwSessionCount);
	pInfo->ulRwSessionCount = long_reverse(pInfo->ulRwSessionCount);
	pInfo->ulMaxPinLen = long_reverse(pInfo->ulMaxPinLen);
	pInfo->ulMinPinLen = long_reverse(pInfo->ulMinPinLen);
	pInfo->ulTotalPublicMemory = long_reverse(pInfo->ulTotalPublicMemory);
	pInfo->ulFreePublicMemory = long_reverse(pInfo->ulFreePublicMemory);
	pInfo->ulTotalPrivateMemory = long_reverse(pInfo->ulTotalPrivateMemory);
	pInfo->ulFreePrivateMemory = long_reverse(pInfo->ulFreePrivateMemory);	
}

CK_RV SC_GetTokenInfo( CK_SLOT_ID         sid,
                       CK_TOKEN_INFO_PTR  pInfo )
{
	CK_RV             rc = CKR_OK;
	time_t now;
	SLT_CHECK;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	if (!pInfo) {
		ock_log_err(OCK_E_FUNC);
		rc = CKR_FUNCTION_FAILED;
		goto done;
	}
	if (slot_id > MAX_SLOT_ID) {
		ock_log_err(OCK_E_SLOT_INV);
		rc = CKR_SLOT_ID_INVALID;
		goto done;
	}
/* TODO: This should always be enabled; eliminate the PKCS64 flag */
#ifdef PKCS64
	copy_token_contents_sensibly(pInfo, nv_token_data);
#else
	memcpy( pInfo, &nv_token_data->token_info, sizeof(CK_TOKEN_INFO) );
#endif


	// Set the time
	now = time ((time_t *)NULL);
	strftime( (char *)pInfo->utcTime, 16, "%X", localtime(&now) );

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x\n", "C_GetTokenInfo", rc );

	}

	UNLOCKIT;
	return rc;
}

CK_RV SC_WaitForSlotEvent( CK_FLAGS        flags,
			   CK_SLOT_ID_PTR  pSlot,
			   CK_VOID_PTR     pReserved )
{
	if (st_Initialized() == FALSE){
		ock_log_err(OCK_E_API_NOT_INIT);
		return CKR_CRYPTOKI_NOT_INITIALIZED;
	}
	st_err_log(142, __FILE__, __LINE__, __FUNCTION__);
	return CKR_FUNCTION_NOT_SUPPORTED;
}

/**
 * For Netscape we want to not support the SSL3 mechs since the native
 * ones perform much better.  Force those slots to be RSA... it's ugly
 * but it works.
 */
static void
netscape_hack(CK_MECHANISM_TYPE_PTR mech_arr_ptr, CK_ULONG count)
{
	char *envrn;
	CK_ULONG i;
	if ((envrn = getenv("NS_SERVER_HOME")) != NULL) {
		for (i = 0; i < count; i++){
			switch (mech_arr_ptr[i]) {
			case CKM_SSL3_PRE_MASTER_KEY_GEN:
			case CKM_SSL3_MASTER_KEY_DERIVE:
			case CKM_SSL3_KEY_AND_MAC_DERIVE:
			case CKM_SSL3_MD5_MAC:
			case CKM_SSL3_SHA1_MAC:
				mech_arr_ptr[i] = CKM_RSA_PKCS;
				break;
			}
		}
	}
}

void mechanism_list_transformations(CK_MECHANISM_TYPE_PTR mech_arr_ptr,
				    CK_ULONG_PTR count_ptr)
{
#ifndef NO_NETSCAPE_HACK
	netscape_hack(mech_arr_ptr, (*count_ptr));
#endif /* #ifndef NO_NETSCAPE_HACK */
}

/**
 * Get the mechanism type list for the current token.
 */
CK_RV SC_GetMechanismList(CK_SLOT_ID sid,
                          CK_MECHANISM_TYPE_PTR pMechList,
                          CK_ULONG_PTR count)
{
	CK_RV rc = CKR_OK;
	SLT_CHECK;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	if (count == NULL) {
		ock_log_err(OCK_E_FUNC);
		rc = CKR_FUNCTION_FAILED;
		goto out;
	}
	if (slot_id > MAX_SLOT_ID) {
		ock_log_err(OCK_E_SLOT_INV);
		rc = CKR_SLOT_ID_INVALID;
		goto out;
	}
	if (!token_specific.t_get_mechanism_list) {
		ock_log_err(OCK_E_FUNC);
		rc = CKR_GENERAL_ERROR;
		goto out;
	}
	rc = token_specific.t_get_mechanism_list(pMechList, count);
	if (rc == CKR_OK) {
		/* To accomodate certain special cases, we may need to
		 * make adjustments to the token's mechanism list. */
		mechanism_list_transformations(pMechList, count);
	}
 out:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x, # mechanisms:  %d\n",
			 "C_GetMechanismList", rc, *count );
	}
	UNLOCKIT;
	return rc;
}

/**
 * Get the mechanism info for the current type and token.
 */
CK_RV SC_GetMechanismInfo(CK_SLOT_ID sid,
                          CK_MECHANISM_TYPE type,
                          CK_MECHANISM_INFO_PTR pInfo)
{
	CK_RV rc = CKR_OK;
	SLT_CHECK;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto out;
	}
	if (pInfo == NULL) {
		ock_log_err(OCK_E_FUNC);
		rc = CKR_FUNCTION_FAILED;
		goto out;
	}
	if (slot_id > MAX_SLOT_ID) {
		ock_log_err(OCK_E_SLOT_INV);
		rc = CKR_SLOT_ID_INVALID;
		goto out;
	}
	if (!token_specific.t_get_mechanism_info) {
		ock_log_err(OCK_E_FUNC);
		rc = CKR_GENERAL_ERROR;
		goto out;
	}
	rc = token_specific.t_get_mechanism_info(type, pInfo);
 out:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x, mech type = 0x%08x\n",
			 "C_GetMechanismInfo", rc, type );
	}
	
	UNLOCKIT;
	return rc;
}

int delete_all_files_in_dir(char *full_dir_path)
{
	return 0;
}

/* This routine should only be called if no other processes are
 * attached to the token.  we need to somehow check that this is the
 * only process Meta API should prevent this since it knows session
 * states in the shared memory. */
CK_RV SC_InitToken( CK_SLOT_ID   sid,
                    CK_CHAR_PTR  pPin,
                    CK_ULONG     ulPinLen,
                    CK_CHAR_PTR  pLabel )
{
	CK_RV      rc = CKR_OK;
	int local_rc = 0;
	CK_BYTE    hash_sha[SHA1_HASH_SIZE];
	CK_SLOT_ID slotID;
	char *s = NULL;
	char *pk_full_path = NULL;
	SLT_CHECK;
	slotID = slot_id;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	if (!pPin || !pLabel) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	if (nv_token_data->token_info.flags & CKF_SO_PIN_LOCKED) {
		ock_log_err(OCK_E_PIN_LOCK);
		rc = CKR_PIN_LOCKED;
		goto done;
	}
	rc = compute_sha( pPin, ulPinLen, hash_sha );
	if (memcmp(nv_token_data->so_pin_sha, hash_sha, SHA1_HASH_SIZE) != 0) {
		ock_log_err(OCK_E_PIN_WRONG);
		rc = CKR_PIN_INCORRECT;
		goto done;
	}
	rc  = token_specific.t_des_key_gen( master_key, MASTER_KEY_SIZE, 3 * DES_KEY_SIZE );
	if (rc != CKR_OK) {
		ock_log_err(OCK_E_FUNC);
		rc = CKR_FUNCTION_FAILED;
		goto done;
	}
	// Before we reconstruct all the data, we should delete the
	// token objects from the filesystem.
	//
	// Construct a string to delete the token objects.
	//
	object_mgr_destroy_token_objects();
#if 0 /* TODO: Implement delete_all_files_in_dir() */
	local_rc = asprintf(&pk_full_path, "%s/%s", pk_dir, PK_LITE_OBJ_DIR);
	if (local_rc == -1) {
		rc = CKR_HOST_MEMORY;
		goto out;
	}
	local_rc = delete_all_files_in_dir(pk_full_path);
	if (local_rc == -1) {
		rc = CKR_FUNCTION_FAILED;
		goto out;
	}
#endif
	local_rc = asprintf(&s, "%s %s/%s/* > /dev/null 2>&1", DEL_CMD, pk_dir,
			    PK_LITE_OBJ_DIR);
	if (local_rc == -1) {
		rc = CKR_HOST_MEMORY;
		goto out;
	}
	system(s);
	free(s);
	s = NULL;
	// META This should be fine since the open session checking
	// should occur at the API not the STDLL
	init_token_data();
	init_slotInfo();
	memcpy( nv_token_data->token_info.label, pLabel, 32 );
	memcpy( nv_token_data->so_pin_sha, hash_sha, SHA1_HASH_SIZE);
	nv_token_data->token_info.flags |= CKF_TOKEN_INITIALIZED;
	rc = save_token_data();
	if (rc != CKR_OK){
		st_err_log(104, __FILE__, __LINE__, __FUNCTION__);
		goto done;
	}
	rc = save_masterkey_so();
	if (rc != CKR_OK){
		st_err_log(149, __FILE__, __LINE__, __FUNCTION__);
		goto done;
	}
 done:
 out:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x\n", "C_InitToken",
			 rc );
	}
	UNLOCKIT;
	if (pk_full_path) {
		free(pk_full_path);
	}
	return rc;
}


//
//
CK_RV SC_InitPIN( ST_SESSION_HANDLE  sSession,
                  CK_CHAR_PTR        pPin,
                  CK_ULONG           ulPinLen )
{
	SESSION         * sess = NULL;
	CK_BYTE           hash_sha[SHA1_HASH_SIZE];
	CK_BYTE           hash_md5[MD5_HASH_SIZE];
	CK_RV             rc = CKR_OK;
	SESS_SET;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	if (!pPin) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}
	if (pin_locked(&sess->session_info, nv_token_data->token_info.flags)
	    == TRUE) {
		ock_log_err(OCK_E_PIN_LOCK);
		rc = CKR_PIN_LOCKED;
		goto done;
	}
	if (sess->session_info.state != CKS_RW_SO_FUNCTIONS) {
		ock_log_err(OCK_E_USER_NOT_LOGGED_IN);
		rc = CKR_USER_NOT_LOGGED_IN;
		goto done;
	}
	if ((ulPinLen < MIN_PIN_LEN) || (ulPinLen > MAX_PIN_LEN)) {
		ock_log_err(OCK_E_PIN_LEN);
		rc = CKR_PIN_LEN_RANGE;
		goto done;
	}
	// compute the SHA and MD5 hashes of the user pin
	rc  = compute_sha( pPin, ulPinLen, hash_sha );
	rc |= compute_md5( pPin, ulPinLen, hash_md5 );
	if (rc != CKR_OK){
		st_err_log(148, __FILE__, __LINE__); 	
		goto done;
	}
	rc = XProcLock( xproclock );
	if (rc != CKR_OK){
		st_err_log(150, __FILE__, __LINE__);
		goto done;
	}
	memcpy(nv_token_data->user_pin_sha, hash_sha, SHA1_HASH_SIZE);
	nv_token_data->token_info.flags |= CKF_USER_PIN_INITIALIZED;
	nv_token_data->token_info.flags &= ~(CKF_USER_PIN_TO_BE_CHANGED);
	nv_token_data->token_info.flags &= ~(CKF_USER_PIN_LOCKED);
	XProcUnLock(xproclock);
	memcpy( user_pin_md5, hash_md5, MD5_HASH_SIZE  );
	rc = save_token_data();
	if (rc != CKR_OK){
		st_err_log(104, __FILE__, __LINE__);
		goto done;
	}
	rc = save_masterkey_user();
	if (rc != CKR_OK){
		st_err_log(149, __FILE__, __LINE__);
	}
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  session = %08x\n", "C_InitPin", rc, 
			 hSession);
	}
	UNLOCKIT;
	return rc;
}

CK_RV SC_SetPIN( ST_SESSION_HANDLE  sSession,
                 CK_CHAR_PTR        pOldPin,
                 CK_ULONG           ulOldLen,
                 CK_CHAR_PTR        pNewPin,
                 CK_ULONG           ulNewLen )
{
	SESSION *sess = NULL;
	CK_BYTE old_hash_sha[SHA1_HASH_SIZE];
	CK_BYTE new_hash_sha[SHA1_HASH_SIZE];
	CK_BYTE hash_md5[MD5_HASH_SIZE];
	CK_RV rc = CKR_OK;
	SESS_SET;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}
	if (pin_locked(&sess->session_info, 
		       nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_LOCK);
		rc = CKR_PIN_LOCKED;
		goto done;
	}
	if ((ulNewLen < MIN_PIN_LEN) || (ulNewLen > MAX_PIN_LEN)) {
		ock_log_err(OCK_E_PIN_LEN);
		rc = CKR_PIN_LEN_RANGE;
		goto done;
	}
	rc = compute_sha( pOldPin, ulOldLen, old_hash_sha );
	if (rc != CKR_OK){
		st_err_log(148, __FILE__, __LINE__); 	
		goto done;
	}
	/* From the PKCS#11 2.20 spec: "C_SetPIN modifies the PIN of
	 * the user that is currently logged in, or the CKU_USER PIN
	 * if the session is not logged in."  A non R/W session fails
	 * with CKR_SESSION_READ_ONLY.
	 */
	if ((sess->session_info.state == CKS_RW_USER_FUNCTIONS) ||
	    (sess->session_info.state == CKS_RW_PUBLIC_SESSION)) {
		if (memcmp(nv_token_data->user_pin_sha, old_hash_sha, 
			   SHA1_HASH_SIZE) != 0) {
			ock_log_err(OCK_E_PIN_WRONG);
			rc = CKR_PIN_INCORRECT;
			goto done;
		}
		rc  = compute_sha( pNewPin, ulNewLen, new_hash_sha );
		rc |= compute_md5( pNewPin, ulNewLen, hash_md5 );
		if (rc != CKR_OK){
			st_err_log(148, __FILE__, __LINE__); 	
			goto done;
		}
		/* The old PIN matches, now make sure its different
		 * than the new and is not the default. */
		if ((memcmp(old_hash_sha, new_hash_sha, SHA1_HASH_SIZE) == 0) ||
		    (memcmp(new_hash_sha, default_user_pin_sha, SHA1_HASH_SIZE)
		     == 0)) {
			ock_log_err(OCK_E_PIN_INV);
			rc = CKR_PIN_INVALID;
			goto done;
		}
		rc = XProcLock( xproclock );
		if (rc != CKR_OK){
			st_err_log(150, __FILE__, __LINE__);
			goto done;
		}
		memcpy(nv_token_data->user_pin_sha, new_hash_sha,
		       SHA1_HASH_SIZE);
		memcpy(user_pin_md5, hash_md5, MD5_HASH_SIZE);
		nv_token_data->token_info.flags &=
			~(CKF_USER_PIN_TO_BE_CHANGED);
		XProcUnLock( xproclock );
		rc = save_token_data();
		if (rc != CKR_OK){
			st_err_log(104, __FILE__, __LINE__);
			goto done;
		}
		rc = save_masterkey_user();
	} else if (sess->session_info.state == CKS_RW_SO_FUNCTIONS) {
		if (memcmp(nv_token_data->so_pin_sha, old_hash_sha, 
			   SHA1_HASH_SIZE) != 0) {
			rc = CKR_PIN_INCORRECT;
			ock_log_err(OCK_E_PIN_WRONG);
			goto done;
		}
		rc = compute_sha(pNewPin, ulNewLen, new_hash_sha);
		rc |= compute_md5(pNewPin, ulNewLen, hash_md5);
		if (rc != CKR_OK){
			st_err_log(148, __FILE__, __LINE__);
			goto done;
		}
		/* The old PIN matches, now make sure its different
		 * than the new and is not the default. */
		if ((memcmp(old_hash_sha, new_hash_sha, SHA1_HASH_SIZE) == 0) ||
		    (memcmp(new_hash_sha, default_so_pin_sha, SHA1_HASH_SIZE)
		     == 0)) {
			ock_log_err(OCK_E_PIN_INV);
			rc = CKR_PIN_INVALID;
			goto done;
		}
		rc = XProcLock( xproclock );
		if (rc != CKR_OK){
			st_err_log(150, __FILE__, __LINE__);
			goto done;
		}
		memcpy(nv_token_data->so_pin_sha, new_hash_sha, SHA1_HASH_SIZE);
		memcpy( so_pin_md5, hash_md5, MD5_HASH_SIZE );
		nv_token_data->token_info.flags &= ~(CKF_SO_PIN_TO_BE_CHANGED);
		XProcUnLock( xproclock );
		rc = save_token_data();
		if (rc != CKR_OK){
			st_err_log(104, __FILE__, __LINE__);
			goto done;
		}
		rc = save_masterkey_so();
	} else {
		st_err_log(142, __FILE__, __LINE__);
		rc = CKR_SESSION_READ_ONLY;
	}
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  session = %08x\n", "C_SetPin", rc,
			 hSession );
	}
	UNLOCKIT;
	if (rc != CKR_SESSION_READ_ONLY && rc != CKR_OK)
		st_err_log(149, __FILE__, __LINE__);
	return rc;
}

CK_RV SC_OpenSession(CK_SLOT_ID             sid,
		     CK_FLAGS               flags,
		     CK_SESSION_HANDLE_PTR  phSession)
{
	SESSION              * sess;
	CK_BBOOL               locked = FALSE;
	CK_RV                  rc = CKR_OK;
	SLT_CHECK;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	if (phSession == NULL) {
		ock_log_err(OCK_E_FUNC);
		rc = CKR_FUNCTION_FAILED;
		goto done;
	}
	if (slot_id > MAX_SLOT_ID) {
		ock_log_err(OCK_E_SLOT_INV);
		rc = CKR_SLOT_ID_INVALID;
		goto done;
	}
	flags |= CKF_SERIAL_SESSION;
	if ((flags & CKF_RW_SESSION) == 0) {
		if (session_mgr_so_session_exists()) {
			ock_log_err(OCK_E_SESS_READWRITE_EXISTS);
			rc = CKR_SESSION_READ_WRITE_SO_EXISTS;
			goto done;
		}
	}
	// Get the mutex because we may modify the pid_list
	rc = MY_LockMutex( &pkcs_mutex );
	if (rc != CKR_OK){
		st_err_log(146, __FILE__, __LINE__); 
		goto done;
	}
	locked = TRUE;
	token_specific.t_session(slot_id);
	MY_UnlockMutex( &pkcs_mutex );
	locked = FALSE;
	rc = session_mgr_new( flags, &sess );
	if (rc != CKR_OK){
		st_err_log(152, __FILE__, __LINE__); 
		goto done;
	}
	*phSession = (CK_SESSION_HANDLE_PTR) sess;
	// Set the correct slot ID here. Was hard coded to 1. - KEY
	sess->session_info.slotID = sid;
 done:
	if (locked)
		MY_UnlockMutex( &pkcs_mutex );
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x  ", "C_OpenSession",
			 rc);
		if (rc == CKR_OK)
			stlogit2(debugfile, "sess = %d", 
				 ((sess == NULL) ? -1 : (CK_LONG)sess->handle));
		stlogit2(debugfile, "\n");
	}
	UNLOCKIT;
	return rc;
}

CK_RV SC_CloseSession( ST_SESSION_HANDLE  sSession )
{
	SESSION  * sess = NULL;
	CK_RV      rc = CKR_OK;
	SESS_SET;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}
	rc = session_mgr_close_session( sess );
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x  sess = %d\n",
			 "C_CloseSession", rc, hSession );
	}
	UNLOCKIT;
	return rc;
}

CK_RV SC_CloseAllSessions( CK_SLOT_ID  sid )
{
	CK_RV rc = CKR_OK;
	SLT_CHECK;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	rc = session_mgr_close_all_sessions();
	if (rc != CKR_OK){
		st_err_log(153, __FILE__, __LINE__);
	}
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x  slot = %d\n",
			 "C_CloseAllSessions", rc, slot_id );
	}
	UNLOCKIT;
	return rc;
}

CK_RV SC_GetSessionInfo( ST_SESSION_HANDLE   sSession,
			 CK_SESSION_INFO_PTR pInfo )
{
	SESSION  * sess = NULL;
	CK_RV      rc = CKR_OK;
	SESS_SET;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pInfo) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	memcpy( pInfo, &sess->session_info, sizeof(CK_SESSION_INFO) );

 done:
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  session = %08d\n",
			 "C_GetSessionInfo", hSession );
	}
	UNLOCKIT;
	return rc;
}

CK_RV SC_GetOperationState( ST_SESSION_HANDLE  sSession,
			    CK_BYTE_PTR        pOperationState,
			    CK_ULONG_PTR       pulOperationStateLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET;
	LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pulOperationStateLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	if (!pOperationState)
		length_only = TRUE;

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}


	rc = session_mgr_get_op_state( sess, length_only,
				       pOperationState,
				       pulOperationStateLen );
	if (rc != CKR_OK){
		st_err_log(154, __FILE__, __LINE__);
	}
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  session = %08x\n", "C_GetOperationState", rc, hSession );
	}

	UNLOCKIT;
	return rc;
}


//
//
CK_RV SC_SetOperationState( ST_SESSION_HANDLE  sSession,
			    CK_BYTE_PTR        pOperationState,
			    CK_ULONG           ulOperationStateLen,
			    CK_OBJECT_HANDLE   hEncryptionKey,
			    CK_OBJECT_HANDLE   hAuthenticationKey )
{
	SESSION  * sess = NULL;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pOperationState || (ulOperationStateLen == 0)) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	rc = session_mgr_set_op_state( sess,
				       hEncryptionKey,  hAuthenticationKey,
				       pOperationState, ulOperationStateLen );

	if (rc != CKR_OK){
		st_err_log(154, __FILE__, __LINE__);
	}
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  session = %08x\n", "C_SetOperationState", rc, hSession );
	}

	UNLOCKIT;
	return rc;
}



//
//
CK_RV SC_Login( ST_SESSION_HANDLE   sSession,
                CK_USER_TYPE        userType,
                CK_CHAR_PTR         pPin,
                CK_ULONG            ulPinLen )
{
	SESSION        * sess = NULL;
	CK_FLAGS_32    * flags = NULL;
	CK_BYTE          hash_sha[SHA1_HASH_SIZE];
	CK_RV            rc = CKR_OK;

	SESS_SET
		LOCKIT;
	// In v2.11, logins should be exclusive, since token
	// specific flags may need to be set for a bad login. - KEY
	rc = MY_LockMutex( &login_mutex );
	if (rc != CKR_OK){
	        st_err_log(146, __FILE__, __LINE__);
		return CKR_FUNCTION_FAILED;
	}
	  
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	
	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}
	flags = &nv_token_data->token_info.flags;

	if (!pPin || ulPinLen > MAX_PIN_LEN) {
		set_login_flags(userType, flags);
		ock_log_err(OCK_E_PIN_WRONG);
		rc = CKR_PIN_INCORRECT;
		goto done;
	}
	
	// PKCS #11 v2.01 requires that all sessions have the same login status:
	//    --> all sessions are public, all are SO or all are USER
	//
	if (userType == CKU_USER) {
		if (session_mgr_so_session_exists()){
			ock_log_err(OCK_E_ANOTHER_USER_ALREADY_LOGGED_IN);
			rc = CKR_USER_ANOTHER_ALREADY_LOGGED_IN;
		}
		if (session_mgr_user_session_exists()){
			ock_log_err(OCK_E_USER_ALREADY_LOGGED_IN);
			rc = CKR_USER_ALREADY_LOGGED_IN;
		}
	}
	else if (userType == CKU_SO) {
		if (session_mgr_user_session_exists()){
			ock_log_err(OCK_E_ANOTHER_USER_ALREADY_LOGGED_IN);
			rc = CKR_USER_ANOTHER_ALREADY_LOGGED_IN;
		}
		if (session_mgr_so_session_exists()){
			ock_log_err(OCK_E_USER_ALREADY_LOGGED_IN);
			rc = CKR_USER_ALREADY_LOGGED_IN;
		}
		if (session_mgr_readonly_session_exists()){
			st_err_log(142, __FILE__, __LINE__);
			rc = CKR_SESSION_READ_ONLY_EXISTS;
		}
	}
	else {
		rc = CKR_USER_TYPE_INVALID;
		ock_log_err(OCK_E_USER_TYPE_INV);
	}
	if (rc != CKR_OK)
		goto done;

	if (userType == CKU_USER) {
		if (*flags & CKF_USER_PIN_LOCKED) {
			ock_log_err(OCK_E_PIN_LOCK);
			rc = CKR_PIN_LOCKED;
			goto done;
		}

		if (memcmp(nv_token_data->user_pin_sha,
			   "00000000000000000000", SHA1_HASH_SIZE) == 0) {
			ock_log_err(OCK_E_PIN_WRONG);
			rc = CKR_USER_PIN_NOT_INITIALIZED;
			goto done;
		}

		rc = compute_sha( pPin, ulPinLen, hash_sha );
		if (memcmp(nv_token_data->user_pin_sha, hash_sha, SHA1_HASH_SIZE) != 0) {
			set_login_flags(userType, flags);
			ock_log_err(OCK_E_PIN_WRONG);
			rc = CKR_PIN_INCORRECT;
			goto done;
		}
		/* Successful login, clear flags */
		*flags &=	~(CKF_USER_PIN_LOCKED |
				  CKF_USER_PIN_FINAL_TRY |
				  CKF_USER_PIN_COUNT_LOW);

		compute_md5( pPin, ulPinLen, user_pin_md5 );
		memset( so_pin_md5, 0x0, MD5_HASH_SIZE );

		rc = load_masterkey_user();
		if (rc != CKR_OK){
			st_err_log(155, __FILE__, __LINE__);
			goto done;
		}
		/* no need to return error here, that would only prevent the stdll from loading. We
		 * load the token objects that we can and syslog the rest */
		load_private_token_objects();

		XProcLock( xproclock );
		global_shm->priv_loaded = TRUE;
		XProcUnLock( xproclock );

	}
	else {
		if (*flags & CKF_SO_PIN_LOCKED) {
			ock_log_err(OCK_E_PIN_LOCK);
			rc = CKR_PIN_LOCKED;
			goto done;
		}
		rc = compute_sha( pPin, ulPinLen, hash_sha );
		if (memcmp(nv_token_data->so_pin_sha, hash_sha, SHA1_HASH_SIZE) != 0) {
			set_login_flags(userType, flags);
			ock_log_err(OCK_E_PIN_WRONG);
			rc = CKR_PIN_INCORRECT;
			goto done;
		}
		/* Successful login, clear flags */
		*flags &= 	~(CKF_SO_PIN_LOCKED | 
				  CKF_SO_PIN_FINAL_TRY | 
				  CKF_SO_PIN_COUNT_LOW);

		compute_md5( pPin, ulPinLen, so_pin_md5 );
		memset( user_pin_md5, 0x0, MD5_HASH_SIZE );

		rc = load_masterkey_so();
		if (rc != CKR_OK) {
			st_err_log(155, __FILE__, __LINE__);
		}
	}

	rc = session_mgr_login_all( userType );
	if (rc != CKR_OK) {
		st_err_log(174, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x\n", "C_Login", rc );
	}

	UNLOCKIT;
	save_token_data();
	MY_UnlockMutex( &login_mutex );
	return rc;
}


//
//
CK_RV SC_Logout( ST_SESSION_HANDLE  sSession )
{
	SESSION  * sess = NULL;
	CK_RV      rc = CKR_OK;

	SESS_SET
	
		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	
	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}
	
	// all sessions have the same state so we just have to check one
	//
	if (session_mgr_public_session_exists()) {
		ock_log_err(OCK_E_USER_NOT_LOGGED_IN);
		rc = CKR_USER_NOT_LOGGED_IN;
		goto done;
	}

	rc = session_mgr_logout_all();
	if (rc != CKR_OK){
		ock_log_err(OCK_E_USER_NOT_LOGGED_IN);
	}
	memset( user_pin_md5, 0x0, MD5_HASH_SIZE );
	memset( so_pin_md5,   0x0, MD5_HASH_SIZE );
	
	object_mgr_purge_private_token_objects();
	
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = 0x%08x\n", "C_Logout", rc );
	}
   
	UNLOCKIT; return rc;
}


// This is a Leeds-Lite solution so we have to store objects on the host.
//
CK_RV SC_CreateObject( ST_SESSION_HANDLE    sSession,
		       CK_ATTRIBUTE_PTR     pTemplate,
		       CK_ULONG             ulCount,
		       CK_OBJECT_HANDLE_PTR phObject )
{
	SESSION               * sess = NULL;
	CK_ULONG                i;
	CK_RV                   rc = CKR_OK;
	SESS_SET

		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	rc = object_mgr_add( sess, pTemplate, ulCount, phObject );
	if (rc != CKR_OK) {
		st_err_log(157, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x\n", "C_CreateObject", rc );

		for (i = 0; i < ulCount; i++) {
			if (pTemplate[i].type == CKA_CLASS)
				stlogit2(debugfile, "%28s:  0x%02x\n", "Object Type", *(CK_ULONG *)pTemplate[i].pValue );
		}
		if (rc == CKR_OK)
			stlogit2(debugfile, "%28s:  %d\n", "Handle", *phObject );

	}

	UNLOCKIT; return rc;
}



//
//
CK_RV  SC_CopyObject( ST_SESSION_HANDLE    sSession,
		      CK_OBJECT_HANDLE     hObject,
		      CK_ATTRIBUTE_PTR     pTemplate,
		      CK_ULONG             ulCount,
		      CK_OBJECT_HANDLE_PTR phNewObject )
{
	SESSION              * sess = NULL;
	CK_RV                  rc = CKR_OK;
	SESS_SET

		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}
   
	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	rc = object_mgr_copy( sess, pTemplate, ulCount, hObject, phNewObject );
	if (rc != CKR_OK) {
		st_err_log(158, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, old handle = %d, new handle = %d\n", "C_CopyObject", rc, hObject, *phNewObject );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DestroyObject( ST_SESSION_HANDLE  sSession,
			CK_OBJECT_HANDLE   hObject )
{
	SESSION               * sess = NULL;
	CK_RV                   rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	rc = object_mgr_destroy_object( sess, hObject );
	if (rc != CKR_OK){
		st_err_log(182, __FILE__, __LINE__);
	}
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, handle = %d\n", "C_DestroyObject", rc, hObject );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_GetObjectSize( ST_SESSION_HANDLE  sSession,
			CK_OBJECT_HANDLE   hObject,
			CK_ULONG_PTR       pulSize )
{
	SESSION               * sess = NULL;
	CK_RV                   rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	rc = object_mgr_get_object_size( hObject, pulSize );
	if (rc != CKR_OK){
		st_err_log(184, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, handle = %d\n", "C_GetObjectSize", rc, hObject );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_GetAttributeValue( ST_SESSION_HANDLE  sSession,
			    CK_OBJECT_HANDLE   hObject,
			    CK_ATTRIBUTE_PTR   pTemplate,
			    CK_ULONG           ulCount )
{
	SESSION        * sess = NULL;
	CK_ATTRIBUTE   * attr = NULL;
	CK_BYTE        * ptr  = NULL;
	CK_ULONG         i;
	CK_RV            rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	rc = object_mgr_get_attribute_values( sess, hObject, pTemplate, ulCount );
	if (rc != CKR_OK){
		st_err_log(159, __FILE__, __LINE__);
	}


 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, handle = %d\n", "C_GetAttributeValue", rc, hObject );

		attr = pTemplate;
		for (i = 0; i < ulCount; i++, attr++) {
			ptr = (CK_BYTE *)attr->pValue;

			stlogit2(debugfile, "   %3d:  Attribute type:  0x%08x\n", i, attr->type );
			stlogit2(debugfile, "         Value Length:    %08d\n",   attr->ulValueLen );

			if (attr->ulValueLen != (CK_ULONG)(-1) && (ptr != NULL))
				stlogit2(debugfile, "         First 4 bytes:   %02x %02x %02x %02x", ptr[0], ptr[1], ptr[2], ptr[3] );

			stlogit2(debugfile, "\n\n");
		}

	}

	UNLOCKIT; return rc;
}


//
//
CK_RV  SC_SetAttributeValue( ST_SESSION_HANDLE    sSession,
			     CK_OBJECT_HANDLE     hObject,
			     CK_ATTRIBUTE_PTR     pTemplate,
			     CK_ULONG             ulCount )
{
	SESSION       * sess = NULL;
	CK_ATTRIBUTE  * attr = NULL;
	CK_ULONG        i;
	CK_RV           rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	rc = object_mgr_set_attribute_values( sess, hObject, pTemplate, ulCount);
	if (rc != CKR_OK){
		st_err_log(161, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, handle = %d\n", "C_SetAttributeValue", rc, hObject );

		attr = pTemplate;
		for (i = 0; i < ulCount; i++, attr++) {
			CK_BYTE *ptr = (CK_BYTE *)attr->pValue;

			stlogit2(debugfile, "   %3d:  Attribute type:  0x%08x\n", i, attr->type );
			stlogit2(debugfile, "         Value Length:    %08d\n",   attr->ulValueLen );

			if (attr->ulValueLen != (CK_ULONG)(-1) && (ptr != NULL))
				stlogit2(debugfile, "         First 4 bytes:   %02x %02x %02x %02x", ptr[0], ptr[1], ptr[2], ptr[3] );

			stlogit2(debugfile, "\n\n");
		}

	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_FindObjectsInit( ST_SESSION_HANDLE   sSession,
			  CK_ATTRIBUTE_PTR    pTemplate,
			  CK_ULONG            ulCount )
{
	SESSION        * sess  = NULL;
	CK_ATTRIBUTE   * attr = NULL;
	CK_ULONG         i;
	CK_RV            rc = CKR_OK;
	SESS_SET

		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	if (sess->find_active == TRUE) {
		ock_log_err(OCK_E_OP_ACTIVE);
		rc = CKR_OPERATION_ACTIVE;
		goto done;
	}

	rc = object_mgr_find_init( sess, pTemplate, ulCount );
	if (rc != CKR_OK){
		st_err_log(185, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x\n", "C_FindObjectsInit", rc );

		attr = pTemplate;
		for (i = 0; i < ulCount; i++, attr++) {
			CK_BYTE *ptr = (CK_BYTE *)attr->pValue;

			stlogit2(debugfile, "   %3d:  Attribute type:  0x%08x\n", i, attr->type );
			stlogit2(debugfile, "         Value Length:    %08d\n",   attr->ulValueLen );

			if (attr->ulValueLen != (CK_ULONG)(-1) && (ptr != NULL))
				stlogit2(debugfile, "         First 4 bytes:   %02x %02x %02x %02x", ptr[0], ptr[1], ptr[2], ptr[3] );

			stlogit2(debugfile, "\n\n");
		}

	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_FindObjects( ST_SESSION_HANDLE     sSession,
		      CK_OBJECT_HANDLE_PTR  phObject,
		      CK_ULONG              ulMaxObjectCount,
		      CK_ULONG_PTR          pulObjectCount )
{
	SESSION    * sess  = NULL;
	CK_ULONG     count = 0;
	CK_RV        rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!phObject || !pulObjectCount) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->find_active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!sess->find_list) {
		ock_log_err(OCK_E_FUNC);
		rc = CKR_FUNCTION_FAILED;
		goto done;
	}
	count = MIN(ulMaxObjectCount, (sess->find_count - sess->find_idx));

	memcpy( phObject, sess->find_list + sess->find_idx, count * sizeof(CK_OBJECT_HANDLE) );
	*pulObjectCount = count;

	sess->find_idx += count;
	rc = CKR_OK;

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, returned %d objects\n", "C_FindObjects", rc, count );
	}

	UNLOCKIT; return rc;
}



//
//
CK_RV SC_FindObjectsFinal( ST_SESSION_HANDLE  sSession )
{
	SESSION     * sess = NULL;
	CK_RV         rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->find_active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (sess->find_list)
		free( sess->find_list );

	sess->find_list   = NULL;
	sess->find_len    = 0;
	sess->find_idx    = 0;
	sess->find_active = FALSE;

	rc = CKR_OK;

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x\n", "C_FindObjectsFinal", rc );
	}

	UNLOCKIT; return rc;
}



//
//
CK_RV SC_EncryptInit( ST_SESSION_HANDLE  sSession,
		      CK_MECHANISM_PTR   pMechanism,
		      CK_OBJECT_HANDLE   hKey )
{
	SESSION               * sess = NULL;
	CK_RV                   rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pMechanism) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	if (sess->encr_ctx.active == TRUE) {
		ock_log_err(OCK_E_OP_ACTIVE);
		rc = CKR_OPERATION_ACTIVE;
		goto done;
	}

	rc = encr_mgr_init( sess, &sess->encr_ctx, OP_ENCRYPT_INIT, pMechanism, hKey );
	if (rc != CKR_OK) {
		ock_log_err(OCK_E_ENCRYPT_MGR_INIT);
	}
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, key = %d, mech = 0x%x\n", "C_EncryptInit", rc,(sess == NULL)?-1:(CK_LONG)sess->handle, hKey, pMechanism->mechanism );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_Encrypt( ST_SESSION_HANDLE  sSession,
		  CK_BYTE_PTR        pData,
		  CK_ULONG           ulDataLen,
		  CK_BYTE_PTR        pEncryptedData,
		  CK_ULONG_PTR       pulEncryptedDataLen )
{
	SESSION        * sess = NULL;
	CK_BBOOL         length_only = FALSE;
	CK_RV            rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pData || !pulEncryptedDataLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->encr_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pEncryptedData)
		length_only = TRUE;

	rc = encr_mgr_encrypt( sess,           length_only,
			       &sess->encr_ctx,
			       pData,          ulDataLen,
			       pEncryptedData, pulEncryptedDataLen );
	if (rc != CKR_OK) {
		ock_log_err(OCK_E_ENCRYPT_MGR_ENCRYPT);
	}

 done:
	LLOCK;
	if (rc != CKR_BUFFER_TOO_SMALL && (rc != CKR_OK || length_only != TRUE))
		encr_mgr_cleanup( &sess->encr_ctx );

	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, amount = %d\n", "C_Encrypt", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulDataLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_EncryptUpdate( ST_SESSION_HANDLE  sSession,
			CK_BYTE_PTR        pPart,
			CK_ULONG           ulPartLen,
			CK_BYTE_PTR        pEncryptedPart,
			CK_ULONG_PTR       pulEncryptedPartLen )
{
	SESSION        * sess = NULL;
	CK_BBOOL         length_only = FALSE;
	CK_RV            rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pPart || !pulEncryptedPartLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->encr_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pEncryptedPart)
		length_only = TRUE;

	rc = encr_mgr_encrypt_update( sess,           length_only,
				      &sess->encr_ctx,
				      pPart,          ulPartLen,
				      pEncryptedPart, pulEncryptedPartLen );
	if (rc != CKR_OK) {
		st_err_log(176, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (rc != CKR_OK && rc != CKR_BUFFER_TOO_SMALL)
		encr_mgr_cleanup( &sess->encr_ctx );

	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, amount = %d\n", "C_EncryptUpdate", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulPartLen );
	}

	UNLOCKIT; return rc;
}


// I think RSA goofed when designing the specification for C_EncryptFinal.
// This function is supposed to follow the Cryptoki standard that if
// pLastEncryptedPart == NULL then the user is requesting only the length
// of the output.
//
// But it's quite possible that no output will be returned (say the user
// specifies a total of 64 bytes of input data throughout the multi-part
// encryption).  The same thing can happen during an EncryptUpdate.
//
// ie:
//
//    1) user calls C_EncryptFinal to get the needed length
//       --> we return "0 bytes required"
//    2) user passes in a NULL pointer for pLastEncryptedPart
//       --> we think the user is requesting the length again <--
//
// So the user needs to pass in a non-NULL pointer even though we're not
// going to return anything in it.  It would have been cleaner if RSA would
// have simply included a "give-me-the-length-only flag" as an argument.
//
//
CK_RV SC_EncryptFinal( ST_SESSION_HANDLE  sSession,
		       CK_BYTE_PTR        pLastEncryptedPart,
		       CK_ULONG_PTR       pulLastEncryptedPartLen )
{
	SESSION     * sess = NULL;
	CK_BBOOL      length_only = FALSE;
	CK_RV         rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pulLastEncryptedPartLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->encr_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pLastEncryptedPart)
		length_only = TRUE;

	rc = encr_mgr_encrypt_final( sess,  length_only, &sess->encr_ctx,
				     pLastEncryptedPart, pulLastEncryptedPartLen );
	if (rc != CKR_OK) {
		st_err_log(177, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (rc != CKR_BUFFER_TOO_SMALL && (rc != CKR_OK || length_only != TRUE))
		encr_mgr_cleanup( &sess->encr_ctx );

	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d\n", "C_EncryptFinal", rc, (sess == NULL)?-1:(CK_LONG)sess->handle );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DecryptInit( ST_SESSION_HANDLE  sSession,
		      CK_MECHANISM_PTR   pMechanism,
		      CK_OBJECT_HANDLE   hKey )
{
	SESSION   * sess = NULL;
	CK_RV       rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pMechanism) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	if (sess->decr_ctx.active == TRUE) {
		ock_log_err(OCK_E_OP_ACTIVE);
		rc = CKR_OPERATION_ACTIVE;
		goto done;
	}

	rc = decr_mgr_init( sess, &sess->decr_ctx, OP_DECRYPT_INIT, pMechanism, hKey );
	if (rc != CKR_OK) {
		st_err_log(179, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, key = %d, mech = 0x%x\n", "C_DecryptInit", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, hKey, pMechanism->mechanism );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_Decrypt( ST_SESSION_HANDLE  sSession,
		  CK_BYTE_PTR        pEncryptedData,
		  CK_ULONG           ulEncryptedDataLen,
		  CK_BYTE_PTR        pData,
		  CK_ULONG_PTR       pulDataLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pEncryptedData || !pulDataLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->decr_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pData)
		length_only = TRUE;

	rc = decr_mgr_decrypt( sess,           length_only,
			       &sess->decr_ctx,
			       pEncryptedData, ulEncryptedDataLen,
			       pData,          pulDataLen );
	if (rc != CKR_OK) {
		st_err_log(100, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (rc != CKR_BUFFER_TOO_SMALL && (rc != CKR_OK || length_only != TRUE))
		decr_mgr_cleanup( &sess->decr_ctx );

	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, amount = %d\n", "C_Decrypt", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulEncryptedDataLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DecryptUpdate( ST_SESSION_HANDLE  sSession,
			CK_BYTE_PTR        pEncryptedPart,
			CK_ULONG           ulEncryptedPartLen,
			CK_BYTE_PTR        pPart,
			CK_ULONG_PTR       pulPartLen )
{
	SESSION   * sess = NULL;
	CK_BBOOL    length_only = FALSE;
	CK_RV       rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pEncryptedPart || !pulPartLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->decr_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pPart)
		length_only = TRUE;

	rc = decr_mgr_decrypt_update( sess,           length_only,
				      &sess->decr_ctx,
				      pEncryptedPart, ulEncryptedPartLen,
				      pPart,          pulPartLen );
	if (rc != CKR_OK) {
		st_err_log(180, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (rc != CKR_OK && rc != CKR_BUFFER_TOO_SMALL)
		decr_mgr_cleanup( &sess->decr_ctx );

	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, amount = %d\n", "C_DecryptUpdate", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulEncryptedPartLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DecryptFinal( ST_SESSION_HANDLE  sSession,
		       CK_BYTE_PTR        pLastPart,
		       CK_ULONG_PTR       pulLastPartLen )
{
	SESSION   * sess = NULL;
	CK_BBOOL    length_only = FALSE;
	CK_RV       rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pulLastPartLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->decr_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pLastPart)
		length_only = TRUE;

	rc = decr_mgr_decrypt_final( sess,      length_only,
				     &sess->decr_ctx,
				     pLastPart, pulLastPartLen );

	if (rc != CKR_OK) {
		st_err_log(181, __FILE__, __LINE__);
	}
 done:
	LLOCK;
	if (rc != CKR_BUFFER_TOO_SMALL && (rc != CKR_OK || length_only != TRUE))
		decr_mgr_cleanup( &sess->decr_ctx );

	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, amount = %d\n", "C_DecryptFinal", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, *pulLastPartLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DigestInit( ST_SESSION_HANDLE  sSession,
		     CK_MECHANISM_PTR   pMechanism )
{
	SESSION   * sess = NULL;
	CK_RV       rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	if (!pMechanism) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	VALID_MECH(pMechanism);


	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	if (sess->digest_ctx.active == TRUE) {
		ock_log_err(OCK_E_OP_ACTIVE);
		rc = CKR_OPERATION_ACTIVE;
		goto done;
	}

	rc = digest_mgr_init( sess, &sess->digest_ctx, pMechanism );
	if (rc != CKR_OK) {
		st_err_log(123, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, mech = %x\n", "C_DigestInit", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, pMechanism->mechanism );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_Digest( ST_SESSION_HANDLE  sSession,
		 CK_BYTE_PTR        pData,
		 CK_ULONG           ulDataLen,
		 CK_BYTE_PTR        pDigest,
		 CK_ULONG_PTR       pulDigestLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	// Netscape has been known to pass a null pData to DigestUpdate
	// but never for Digest.  It doesn't really make sense to allow it here
	//
	if (!pData || !pulDigestLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->digest_ctx.active == FALSE) {
		ock_log_err(OCK_E_FUNCTION_NOT_PERMITTED);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pDigest)
		length_only = TRUE;

	rc = digest_mgr_digest( sess,    length_only,
				&sess->digest_ctx,
				pData,   ulDataLen,
				pDigest, pulDigestLen );
	if (rc != CKR_OK) {
		st_err_log(124, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, datalen = %d\n", "C_Digest", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulDataLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DigestUpdate( ST_SESSION_HANDLE  sSession,
		       CK_BYTE_PTR        pPart,
		       CK_ULONG           ulPartLen )
{
	SESSION  * sess = NULL;
	CK_RV      rc   = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	// Netscape has been known to pass a null pPart with ulPartLen == 0...
	//
	if (!pPart && ulPartLen != 0) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->digest_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (pPart){
		rc = digest_mgr_digest_update( sess, &sess->digest_ctx, pPart, ulPartLen );
		if (rc != CKR_OK) {
			st_err_log(124, __FILE__, __LINE__);
		}
	}
 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, datalen = %d\n", "C_DigestUpdate", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulPartLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DigestKey( ST_SESSION_HANDLE  sSession,
		    CK_OBJECT_HANDLE   hKey )
{
	SESSION  * sess = NULL;
	CK_RV      rc = CKR_OK;
	SESS_SET

		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->digest_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	rc = digest_mgr_digest_key( sess, &sess->digest_ctx, hKey );
	if (rc != CKR_OK){ 
		st_err_log(124, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, key = %d\n", "C_DigestKey", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, hKey );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DigestFinal( ST_SESSION_HANDLE  sSession,
		      CK_BYTE_PTR        pDigest,
		      CK_ULONG_PTR       pulDigestLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pulDigestLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->digest_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pDigest)
		length_only = TRUE;

	rc = digest_mgr_digest_final( sess,    length_only,
				      &sess->digest_ctx,
				      pDigest, pulDigestLen );
	if (rc != CKR_OK){ 
		st_err_log(126, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d\n", "C_DigestFinal", rc, (sess == NULL)?-1:(CK_LONG)sess->handle );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_SignInit( ST_SESSION_HANDLE  sSession,
		   CK_MECHANISM_PTR   pMechanism,
		   CK_OBJECT_HANDLE   hKey )
{
	SESSION   * sess = NULL;
	CK_RV       rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pMechanism ){
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}
	VALID_MECH(pMechanism);

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	if (sess->sign_ctx.active == TRUE) {
		rc = CKR_OPERATION_ACTIVE;
		ock_log_err(OCK_E_OP_ACTIVE);
		goto done;
	}

	rc = sign_mgr_init( sess, &sess->sign_ctx, pMechanism, FALSE, hKey );
	if (rc != CKR_OK){ 
		st_err_log(127, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, mech = %x\n", "C_SignInit", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, pMechanism->mechanism );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_Sign( ST_SESSION_HANDLE  sSession,
	       CK_BYTE_PTR        pData,
	       CK_ULONG           ulDataLen,
	       CK_BYTE_PTR        pSignature,
	       CK_ULONG_PTR       pulSignatureLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pData || !pulSignatureLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->sign_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pSignature)
		length_only = TRUE;

	rc = sign_mgr_sign( sess,       length_only,
			    &sess->sign_ctx,
			    pData,      ulDataLen,
			    pSignature, pulSignatureLen );
	if (rc != CKR_OK){ 
		st_err_log(171, __FILE__, __LINE__);
	}

 done:
	if (rc != CKR_BUFFER_TOO_SMALL && (rc != CKR_OK || length_only != TRUE))
		sign_mgr_cleanup( &sess->sign_ctx );

	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, datalen = %d\n", "C_Sign", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulDataLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_SignUpdate( ST_SESSION_HANDLE  sSession,
		     CK_BYTE_PTR        pPart,
		     CK_ULONG           ulPartLen )
{
	SESSION  * sess = NULL;
	CK_RV      rc   = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pPart) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->sign_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	rc = sign_mgr_sign_update( sess, &sess->sign_ctx, pPart, ulPartLen );
	if (rc != CKR_OK){ 
		st_err_log(128, __FILE__, __LINE__);
	}

 done:
	if (rc != CKR_OK)
		sign_mgr_cleanup( &sess->sign_ctx );

	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, datalen = %d\n", "C_SignUpdate", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulPartLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_SignFinal( ST_SESSION_HANDLE  sSession,
		    CK_BYTE_PTR        pSignature,
		    CK_ULONG_PTR       pulSignatureLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pulSignatureLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->sign_ctx.active == FALSE) {
		ock_log_err(OCK_E_OP_NOT_INIT);
		rc = CKR_OPERATION_NOT_INITIALIZED;
		goto done;
	}

	if (!pSignature)
		length_only = TRUE;

	rc = sign_mgr_sign_final( sess,       length_only,
				  &sess->sign_ctx,
				  pSignature, pulSignatureLen );
	if (rc != CKR_OK){ 
		st_err_log(129, __FILE__, __LINE__);
	}

 done:
	if (rc != CKR_BUFFER_TOO_SMALL && (rc != CKR_OK || length_only != TRUE))
		sign_mgr_cleanup( &sess->sign_ctx );

	LLOCK;
	if (debugfile)
	{
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d\n", "C_SignFinal", rc, (sess == NULL)?-1:(CK_LONG)sess->handle );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_SignRecoverInit( ST_SESSION_HANDLE  sSession,
			  CK_MECHANISM_PTR   pMechanism,
			  CK_OBJECT_HANDLE   hKey )
{
	SESSION   * sess = NULL;
	CK_RV       rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	if (!pMechanism ){
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	if (sess->sign_ctx.active == TRUE) {
		rc = CKR_OPERATION_ACTIVE;
		ock_log_err(OCK_E_OP_ACTIVE);
		goto done;
	}

	rc = sign_mgr_init( sess, &sess->sign_ctx, pMechanism, TRUE, hKey );
	if (rc != CKR_OK){ 
		st_err_log(127, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, mech = %x\n", "C_SignRecoverInit", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, pMechanism->mechanism );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_SignRecover( ST_SESSION_HANDLE  sSession,
		      CK_BYTE_PTR        pData,
		      CK_ULONG           ulDataLen,
		      CK_BYTE_PTR        pSignature,
		      CK_ULONG_PTR       pulSignatureLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET


		if (st_Initialized() == FALSE) {
			ock_log_err(OCK_E_API_NOT_INIT);
			LOCKIT;
			rc = CKR_CRYPTOKI_NOT_INITIALIZED;
			goto done;
		}

	if (!pData || !pulSignatureLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if ((sess->sign_ctx.active == FALSE) || (sess->sign_ctx.recover == FALSE)) {
		rc = CKR_OPERATION_NOT_INITIALIZED;
		ock_log_err(OCK_E_OP_NOT_INIT);
		goto done;
	}

	if (!pSignature)
		length_only = TRUE;

	rc = sign_mgr_sign_recover( sess,       length_only,
				    &sess->sign_ctx,
				    pData,      ulDataLen,
				    pSignature, pulSignatureLen );
	if (rc != CKR_OK){ 
		st_err_log(186, __FILE__, __LINE__);
	}

 done:
	if (rc != CKR_BUFFER_TOO_SMALL && (rc != CKR_OK || length_only != TRUE))
		sign_mgr_cleanup( &sess->sign_ctx );

	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, datalen = %d\n", "C_SignRecover", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulDataLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_VerifyInit( ST_SESSION_HANDLE  sSession,
		     CK_MECHANISM_PTR   pMechanism,
		     CK_OBJECT_HANDLE   hKey )
{
	SESSION   * sess = NULL;
	CK_RV       rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	if (!pMechanism ){
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	if (sess->verify_ctx.active == TRUE) {
		rc = CKR_OPERATION_ACTIVE;
		ock_log_err(OCK_E_OP_ACTIVE);
		goto done;
	}

	rc = verify_mgr_init( sess, &sess->verify_ctx, pMechanism, FALSE, hKey );
	if (rc != CKR_OK){ 
		st_err_log(167, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, mech = %x\n", "C_VerifyInit", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, pMechanism->mechanism );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_Verify( ST_SESSION_HANDLE  sSession,
		 CK_BYTE_PTR        pData,
		 CK_ULONG           ulDataLen,
		 CK_BYTE_PTR        pSignature,
		 CK_ULONG           ulSignatureLen )
{
	SESSION  * sess = NULL;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pData || !pSignature) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->verify_ctx.active == FALSE) {
		rc = CKR_OPERATION_NOT_INITIALIZED;
		ock_log_err(OCK_E_OP_NOT_INIT);
		goto done;
	}

	rc = verify_mgr_verify( sess,
				&sess->verify_ctx,
				pData,      ulDataLen,
				pSignature, ulSignatureLen );
	if (rc != CKR_OK){ 
		st_err_log(168, __FILE__, __LINE__);
	}

 done:
	verify_mgr_cleanup( &sess->verify_ctx );

	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, datalen = %d\n", "C_Verify", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulDataLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_VerifyUpdate( ST_SESSION_HANDLE  sSession,
		       CK_BYTE_PTR        pPart,
		       CK_ULONG           ulPartLen )
{
	SESSION  * sess = NULL;
	CK_RV      rc   = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pPart) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->verify_ctx.active == FALSE) {
		rc = CKR_OPERATION_NOT_INITIALIZED;
		ock_log_err(OCK_E_OP_NOT_INIT);
		goto done;
	}

	rc = verify_mgr_verify_update( sess, &sess->verify_ctx, pPart, ulPartLen );
	if (rc != CKR_OK){ 
		st_err_log(169, __FILE__, __LINE__);
	}

 done:
	if (rc != CKR_OK)
		verify_mgr_cleanup( &sess->verify_ctx );

	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, datalen = %d\n", "C_VerifyUpdate", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, ulPartLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_VerifyFinal( ST_SESSION_HANDLE  sSession,
		      CK_BYTE_PTR        pSignature,
		      CK_ULONG           ulSignatureLen )
{
	SESSION  * sess = NULL;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pSignature) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (sess->verify_ctx.active == FALSE) {
		rc = CKR_OPERATION_NOT_INITIALIZED;
		ock_log_err(OCK_E_OP_NOT_INIT);
		goto done;
	}

	rc = verify_mgr_verify_final( sess, &sess->verify_ctx, pSignature, ulSignatureLen );
	if (rc != CKR_OK){ 
		st_err_log(170, __FILE__, __LINE__);
	}

 done:
	verify_mgr_cleanup( &sess->verify_ctx );

	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d\n", "C_VerifyFinal", rc, (sess == NULL)?-1:(CK_LONG)sess->handle );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_VerifyRecoverInit( ST_SESSION_HANDLE  sSession,
			    CK_MECHANISM_PTR   pMechanism,
			    CK_OBJECT_HANDLE   hKey )
{
	SESSION   * sess = NULL;
	CK_RV       rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}
	if (!pMechanism ){
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	if (sess->verify_ctx.active == TRUE) {
		rc = CKR_OPERATION_ACTIVE;
		ock_log_err(OCK_E_OP_ACTIVE);
		goto done;
	}

	rc = verify_mgr_init( sess, &sess->verify_ctx, pMechanism, TRUE, hKey );
	if (rc != CKR_OK){ 
		st_err_log(167, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, mech = %x\n", "C_VerifyRecoverInit", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, pMechanism->mechanism );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_VerifyRecover( ST_SESSION_HANDLE  sSession,
			CK_BYTE_PTR        pSignature,
			CK_ULONG           ulSignatureLen,
			CK_BYTE_PTR        pData,
			CK_ULONG_PTR       pulDataLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pSignature || !pulDataLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if ((sess->verify_ctx.active == FALSE) || (sess->verify_ctx.recover == FALSE)) {
		rc = CKR_OPERATION_NOT_INITIALIZED;
		ock_log_err(OCK_E_OP_NOT_INIT);
		goto done;
	}


	if (!pData)
		length_only = TRUE;

	rc = verify_mgr_verify_recover( sess,       length_only,
					&sess->verify_ctx,
					pSignature, ulSignatureLen,
					pData,      pulDataLen );
	if (rc != CKR_OK){ 
		st_err_log(187, __FILE__, __LINE__);
	}

 done:
	if (rc != CKR_BUFFER_TOO_SMALL && (rc != CKR_OK || length_only != TRUE))
		verify_mgr_cleanup( &sess->verify_ctx );

	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, recover len = %d, length_only = %d\n", "C_VerifyRecover", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, *pulDataLen, length_only );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DigestEncryptUpdate( ST_SESSION_HANDLE  sSession,
			      CK_BYTE_PTR        pPart,
			      CK_ULONG           ulPartLen,
			      CK_BYTE_PTR        pEncryptedPart,
			      CK_ULONG_PTR       pulEncryptedPartLen )
{
	SESS_SET
		if (st_Initialized() == FALSE){
			ock_log_err(OCK_E_API_NOT_INIT);
			return CKR_CRYPTOKI_NOT_INITIALIZED;
		}
	st_err_log(142, __FILE__, __LINE__, __FUNCTION__);
	return CKR_FUNCTION_NOT_SUPPORTED;
}


//
//
CK_RV SC_DecryptDigestUpdate( ST_SESSION_HANDLE  sSession,
			      CK_BYTE_PTR        pEncryptedPart,
			      CK_ULONG           ulEncryptedPartLen,
			      CK_BYTE_PTR        pPart,
			      CK_ULONG_PTR       pulPartLen )
{
	SESS_SET
		if (st_Initialized() == FALSE){
			ock_log_err(OCK_E_API_NOT_INIT);
			return CKR_CRYPTOKI_NOT_INITIALIZED;
		}

	st_err_log(142, __FILE__, __LINE__, __FUNCTION__);
	return CKR_FUNCTION_NOT_SUPPORTED;
}


//
//
CK_RV SC_SignEncryptUpdate( ST_SESSION_HANDLE  sSession,
			    CK_BYTE_PTR        pPart,
			    CK_ULONG           ulPartLen,
			    CK_BYTE_PTR        pEncryptedPart,
			    CK_ULONG_PTR       pulEncryptedPartLen )
{
	SESS_SET
		if (st_Initialized() == FALSE){
			ock_log_err(OCK_E_API_NOT_INIT);
			return CKR_CRYPTOKI_NOT_INITIALIZED;
		}
	st_err_log(142, __FILE__, __LINE__, __FUNCTION__);
	return CKR_FUNCTION_NOT_SUPPORTED;
}


//
//
CK_RV SC_DecryptVerifyUpdate( ST_SESSION_HANDLE  sSession,
			      CK_BYTE_PTR        pEncryptedPart,
			      CK_ULONG           ulEncryptedPartLen,
			      CK_BYTE_PTR        pPart,
			      CK_ULONG_PTR       pulPartLen )
{
	SESS_SET
		if (st_Initialized() == FALSE){
			ock_log_err(OCK_E_API_NOT_INIT);
			return CKR_CRYPTOKI_NOT_INITIALIZED;
		}
	st_err_log(142, __FILE__, __LINE__, __FUNCTION__);
	return CKR_FUNCTION_NOT_SUPPORTED;
}


//
//
CK_RV SC_GenerateKey( ST_SESSION_HANDLE     sSession,
		      CK_MECHANISM_PTR      pMechanism,
		      CK_ATTRIBUTE_PTR      pTemplate,
		      CK_ULONG              ulCount,
		      CK_OBJECT_HANDLE_PTR  phKey )
{
	SESSION       * sess = NULL;
	CK_RV           rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pMechanism || !phKey || (pTemplate == NULL && ulCount != 0)) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	rc = key_mgr_generate_key( sess, pMechanism, pTemplate, ulCount, phKey );
	if (rc != CKR_OK){ 
		ock_log_err(OCK_E_KEY_GENERATION);
	}

 done:
	LLOCK;
	if (debugfile) {
		CK_ATTRIBUTE *attr = pTemplate;
		CK_ULONG      i;

		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, handle = %d, mech = %x\n", "C_GenerateKey", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, *phKey, pMechanism->mechanism );

		for (i = 0; i < ulCount; i++, attr++) {
			CK_BYTE *ptr = (CK_BYTE *)attr->pValue;

			stlogit2(debugfile, "   %3d:  Attribute type:  0x%08x\n", i, attr->type );
			stlogit2(debugfile, "         Value Length:    %08d\n",   attr->ulValueLen );

			if (attr->ulValueLen != (CK_ULONG)(-1) && (ptr != NULL))
				stlogit2(debugfile, "         First 4 bytes:   %02x %02x %02x %02x", ptr[0], ptr[1], ptr[2], ptr[3] );

			stlogit2(debugfile, "\n\n");
		}

	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_GenerateKeyPair( ST_SESSION_HANDLE     sSession,
			  CK_MECHANISM_PTR      pMechanism,
			  CK_ATTRIBUTE_PTR      pPublicKeyTemplate,
			  CK_ULONG              ulPublicKeyAttributeCount,
			  CK_ATTRIBUTE_PTR      pPrivateKeyTemplate,
			  CK_ULONG              ulPrivateKeyAttributeCount,
			  CK_OBJECT_HANDLE_PTR  phPublicKey,
			  CK_OBJECT_HANDLE_PTR  phPrivateKey )
{
	SESSION       * sess = NULL;
	CK_RV           rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pMechanism || !phPublicKey || !phPrivateKey ||
	    (!pPublicKeyTemplate && (ulPublicKeyAttributeCount != 0)) ||
	    (!pPrivateKeyTemplate && (ulPrivateKeyAttributeCount != 0)))
	{
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	rc = key_mgr_generate_key_pair( sess,                pMechanism,
					pPublicKeyTemplate,  ulPublicKeyAttributeCount,
					pPrivateKeyTemplate, ulPrivateKeyAttributeCount,
					phPublicKey,         phPrivateKey );
	if (rc != CKR_OK){ 
		ock_log_err(OCK_E_KEY_GENERATION);
	}

 done:
	LLOCK;
	if (debugfile) {
		CK_ATTRIBUTE *attr = NULL;
		CK_ULONG      i;

		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, mech = %x\n", "C_GenerateKeyPair", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, pMechanism->mechanism );

		if (rc == CKR_OK) {
			stlogit2(debugfile, "   Public  handle:  %d\n", *phPublicKey );
			stlogit2(debugfile, "   Private handle:  %d\n", *phPrivateKey );
		}

		stlogit2(debugfile, "   Public Template:\n");

		attr = pPublicKeyTemplate;
		for (i = 0; i < ulPublicKeyAttributeCount; i++, attr++) {
			CK_BYTE *ptr = (CK_BYTE *)attr->pValue;

			stlogit2(debugfile, "   %3d:  Attribute type:  0x%08x\n", i, attr->type );
			stlogit2(debugfile, "         Value Length:    %08d\n",   attr->ulValueLen );

			if (attr->ulValueLen != (CK_ULONG)(-1) && (ptr != NULL))
				stlogit2(debugfile, "         First 4 bytes:   %02x %02x %02x %02x", ptr[0], ptr[1], ptr[2], ptr[3] );

			stlogit2(debugfile, "\n\n");
		}

		stlogit2(debugfile, "   Private Template:\n");

		attr = pPrivateKeyTemplate;
		for (i = 0; i < ulPrivateKeyAttributeCount; i++, attr++) {
			CK_BYTE *ptr = (CK_BYTE *)attr->pValue;

			stlogit2(debugfile, "   %3d:  Attribute type:  0x%08x\n", i, attr->type );
			stlogit2(debugfile, "         Value Length:    %08d\n",   attr->ulValueLen );

			if (attr->ulValueLen != (CK_ULONG)(-1) && (ptr != NULL))
				stlogit2(debugfile, "         First 4 bytes:   %02x %02x %02x %02x", ptr[0], ptr[1], ptr[2], ptr[3] );

			stlogit2(debugfile, "\n\n");
		}

	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_WrapKey( ST_SESSION_HANDLE  sSession,
		  CK_MECHANISM_PTR   pMechanism,
		  CK_OBJECT_HANDLE   hWrappingKey,
		  CK_OBJECT_HANDLE   hKey,
		  CK_BYTE_PTR        pWrappedKey,
		  CK_ULONG_PTR       pulWrappedKeyLen )
{
	SESSION  * sess = NULL;
	CK_BBOOL   length_only = FALSE;
	CK_RV      rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pMechanism || !pulWrappedKeyLen) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	if (!pWrappedKey)
		length_only = TRUE;

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	rc = key_mgr_wrap_key( sess,         length_only,
			       pMechanism,
			       hWrappingKey, hKey,
			       pWrappedKey,  pulWrappedKeyLen );
	if (rc != CKR_OK){ 
		st_err_log(188, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, encrypting key = %d, wrapped key = %d\n", "C_WrapKey", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, hWrappingKey, hKey );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_UnwrapKey( ST_SESSION_HANDLE     sSession,
		    CK_MECHANISM_PTR      pMechanism,
		    CK_OBJECT_HANDLE      hUnwrappingKey,
		    CK_BYTE_PTR           pWrappedKey,
		    CK_ULONG              ulWrappedKeyLen,
		    CK_ATTRIBUTE_PTR      pTemplate,
		    CK_ULONG              ulCount,
		    CK_OBJECT_HANDLE_PTR  phKey )
{
	SESSION        * sess = NULL;
	CK_ATTRIBUTE   * attr = NULL;
	CK_BYTE        * ptr  = NULL;
	CK_ULONG         i;
	CK_RV            rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pMechanism || !pWrappedKey ||
	    (!pTemplate && ulCount != 0) || !phKey)
	{
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	rc = key_mgr_unwrap_key( sess,           pMechanism,
				 pTemplate,      ulCount,
				 pWrappedKey,    ulWrappedKeyLen,
				 hUnwrappingKey, phKey );
	if (rc != CKR_OK){ 
		st_err_log(189, __FILE__, __LINE__);
	}

 done:
//   if (rc == CKR_OBJECT_HANDLE_INVALID)  brkpt();
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, decrypting key = %d, unwrapped key = %d\n", "C_UnwrapKey", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, hUnwrappingKey, *phKey );

		attr = pTemplate;
		for (i = 0; i < ulCount; i++, attr++) {
			ptr = (CK_BYTE *)attr->pValue;

			stlogit2(debugfile, "   %3d:  Attribute type:  0x%08x\n", i, attr->type );
			stlogit2(debugfile, "         Value Length:    %08d\n",   attr->ulValueLen );

			if (attr->ulValueLen != (CK_ULONG)(-1) && (ptr != NULL))
				stlogit2(debugfile, "         First 4 bytes:   %02x %02x %02x %02x", ptr[0], ptr[1], ptr[2], ptr[3] );

			stlogit2(debugfile, "\n\n");
		}

	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_DeriveKey( ST_SESSION_HANDLE     sSession,
		    CK_MECHANISM_PTR      pMechanism,
		    CK_OBJECT_HANDLE      hBaseKey,
		    CK_ATTRIBUTE_PTR      pTemplate,
		    CK_ULONG              ulCount,
		    CK_OBJECT_HANDLE_PTR  phKey )
{
	SESSION        * sess = NULL;
	CK_ATTRIBUTE   * attr = NULL;
	CK_BYTE        * ptr  = NULL;
	CK_ULONG         i;
	CK_RV            rc = CKR_OK;
	SESS_SET


		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pMechanism || (!pTemplate && ulCount != 0)) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}
	VALID_MECH(pMechanism);

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	if (pin_expired(&sess->session_info, nv_token_data->token_info.flags) == TRUE) {
		ock_log_err(OCK_E_PIN_EXP);
		rc = CKR_PIN_EXPIRED;
		goto done;
	}
   
	rc = key_mgr_derive_key( sess,      pMechanism,
				 hBaseKey,  phKey,
				 pTemplate, ulCount );
	if (rc != CKR_OK){ 
		st_err_log(190, __FILE__, __LINE__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, sess = %d, base key = %d, mech = %x\n", "C_DeriveKey", rc, (sess == NULL)?-1:(CK_LONG)sess->handle, hBaseKey, pMechanism->mechanism );

		if (rc == CKR_OK) {
			switch (pMechanism->mechanism) {
			case CKM_SSL3_KEY_AND_MAC_DERIVE:
			{
				CK_SSL3_KEY_MAT_PARAMS *pReq;
				CK_SSL3_KEY_MAT_OUT    *pPtr;
				pReq = (CK_SSL3_KEY_MAT_PARAMS *)pMechanism->pParameter;
				pPtr = pReq->pReturnedKeyMaterial;

				stlogit2(debugfile, "   Client MAC key:  %d\n", pPtr->hClientMacSecret );
				stlogit2(debugfile, "   Server MAC key:  %d\n", pPtr->hServerMacSecret );
				stlogit2(debugfile, "   Client Key:      %d\n", pPtr->hClientKey );
				stlogit2(debugfile, "   Server Key:      %d\n", pPtr->hServerKey );
			}
			break;

			case CKM_DH_PKCS_DERIVE:
			{
				stlogit2(debugfile, "   DH Shared Secret:  \n" );
			}
			break ;

			default:
				stlogit2(debugfile, "   Derived key:     %d\n", *phKey );
			}
		}


		attr = pTemplate;
		for (i = 0; i < ulCount; i++, attr++) {
			ptr = (CK_BYTE *)attr->pValue;

			stlogit2(debugfile, "   %3d:  Attribute type:  0x%08x\n", i, attr->type );
			stlogit2(debugfile, "         Value Length:    %08d\n",   attr->ulValueLen );

			if (attr->ulValueLen != (CK_ULONG)(-1) && (ptr != NULL))
				stlogit2(debugfile, "         First 4 bytes:   %02x %02x %02x %02x", ptr[0], ptr[1], ptr[2], ptr[3] );

			stlogit2(debugfile, "\n\n");
		}

	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_SeedRandom( ST_SESSION_HANDLE  sSession,
		     CK_BYTE_PTR        pSeed,
		     CK_ULONG           ulSeedLen )
{
	SESS_SET
		if (st_Initialized() == FALSE){
			ock_log_err(OCK_E_API_NOT_INIT);
			return CKR_CRYPTOKI_NOT_INITIALIZED;
		}
	return CKR_OK;
}


//
//
CK_RV SC_GenerateRandom( ST_SESSION_HANDLE  sSession,
			 CK_BYTE_PTR        pRandomData,
			 CK_ULONG           ulRandomLen )
{
	SESSION *sess = NULL;
	CK_RV    rc = CKR_OK;
	SESS_SET

		LOCKIT;
	if (st_Initialized() == FALSE) {
		ock_log_err(OCK_E_API_NOT_INIT);
		rc = CKR_CRYPTOKI_NOT_INITIALIZED;
		goto done;
	}

	if (!pRandomData && ulRandomLen != 0) {
		ock_log_err(OCK_E_BAD_ARG);
		rc = CKR_ARGUMENTS_BAD;
		goto done;
	}

	sess = SESSION_MGR_FIND( hSession );
	if (!sess) {
		ock_log_err(OCK_E_SESS_HANDLE_INV);
		rc = CKR_SESSION_HANDLE_INVALID;
		goto done;
	}

	rc = rng_generate( pRandomData, ulRandomLen );
	if (rc != CKR_OK){ 
		st_err_log(130, __FILE__, __LINE__, __FUNCTION__);
	}

 done:
	LLOCK;
	if (debugfile) {
		stlogit2(debugfile, "%-25s:  rc = %08x, %d bytes\n", "C_GenerateRandom", rc, ulRandomLen );
	}

	UNLOCKIT; return rc;
}


//
//
CK_RV SC_GetFunctionStatus( ST_SESSION_HANDLE  sSession )
{
	SESS_SET
		if (st_Initialized() == FALSE){
			ock_log_err(OCK_E_API_NOT_INIT);
			return CKR_CRYPTOKI_NOT_INITIALIZED;
		}
	ock_log_err(OCK_E_FUNC_NOT_PARALLEL);
	return CKR_FUNCTION_NOT_PARALLEL;
}


//
//
CK_RV SC_CancelFunction( ST_SESSION_HANDLE  sSession )
{
	SESS_SET
		if (st_Initialized() == FALSE){
			ock_log_err(OCK_E_API_NOT_INIT);
			return CKR_CRYPTOKI_NOT_INITIALIZED;
		}
	ock_log_err(OCK_E_FUNC_NOT_PARALLEL);
	return CKR_FUNCTION_NOT_PARALLEL;
}

//
//
CK_RV QueryTweakValues( void )
{
	st_err_log(142, __FILE__, __LINE__, __FUNCTION__);
	return CKR_FUNCTION_NOT_SUPPORTED;
}


//
//
CK_RV UpdateTweakValues( void )
{
	st_err_log(142, __FILE__, __LINE__, __FUNCTION__);
	return CKR_FUNCTION_NOT_SUPPORTED;
}

// Added for AIX work
void
SC_SetFunctionList(void){

	function_list.ST_Initialize          = (void *)ST_Initialize;
	function_list.ST_GetTokenInfo        = SC_GetTokenInfo;
	function_list.ST_GetMechanismList    = SC_GetMechanismList;
	function_list.ST_GetMechanismInfo    = SC_GetMechanismInfo;
	function_list.ST_InitToken           = SC_InitToken;
	function_list.ST_InitPIN             = SC_InitPIN;
	function_list.ST_SetPIN              = SC_SetPIN;
	function_list.ST_OpenSession         = SC_OpenSession;
	function_list.ST_CloseSession        = SC_CloseSession;
	function_list.ST_GetSessionInfo      = SC_GetSessionInfo;
	function_list.ST_GetOperationState   = SC_GetOperationState;
	function_list.ST_SetOperationState   = SC_SetOperationState;
	function_list.ST_Login               = SC_Login;
	function_list.ST_Logout              = SC_Logout;
	function_list.ST_CreateObject        = SC_CreateObject;
	function_list.ST_CopyObject          = SC_CopyObject;
	function_list.ST_DestroyObject       = SC_DestroyObject;
	function_list.ST_GetObjectSize       = SC_GetObjectSize;
	function_list.ST_GetAttributeValue   = SC_GetAttributeValue;
	function_list.ST_SetAttributeValue   = SC_SetAttributeValue;
	function_list.ST_FindObjectsInit     = SC_FindObjectsInit;
	function_list.ST_FindObjects         = SC_FindObjects;
	function_list.ST_FindObjectsFinal    = SC_FindObjectsFinal;
	function_list.ST_EncryptInit         = SC_EncryptInit;
	function_list.ST_Encrypt             = SC_Encrypt;
	function_list.ST_EncryptUpdate       = SC_EncryptUpdate;
	function_list.ST_EncryptFinal        = SC_EncryptFinal;
	function_list.ST_DecryptInit         = SC_DecryptInit;
	function_list.ST_Decrypt             = SC_Decrypt;
	function_list.ST_DecryptUpdate       = SC_DecryptUpdate;
	function_list.ST_DecryptFinal        = SC_DecryptFinal;
	function_list.ST_DigestInit          = SC_DigestInit;
	function_list.ST_Digest              = SC_Digest;
	function_list.ST_DigestUpdate        = SC_DigestUpdate;
	function_list.ST_DigestKey           = SC_DigestKey;
	function_list.ST_DigestFinal         = SC_DigestFinal;
	function_list.ST_SignInit            = SC_SignInit;
	function_list.ST_Sign                = SC_Sign;
	function_list.ST_SignUpdate          = SC_SignUpdate;
	function_list.ST_SignFinal           = SC_SignFinal;
	function_list.ST_SignRecoverInit     = SC_SignRecoverInit;
	function_list.ST_SignRecover         = SC_SignRecover;
	function_list.ST_VerifyInit          = SC_VerifyInit;
	function_list.ST_Verify              = SC_Verify;
	function_list.ST_VerifyUpdate        = SC_VerifyUpdate;
	function_list.ST_VerifyFinal         = SC_VerifyFinal;
	function_list.ST_VerifyRecoverInit   = SC_VerifyRecoverInit;
	function_list.ST_VerifyRecover       = SC_VerifyRecover;
	function_list.ST_DigestEncryptUpdate = NULL; // SC_DigestEncryptUpdate;
	function_list.ST_DecryptDigestUpdate = NULL; // SC_DecryptDigestUpdate;
	function_list.ST_SignEncryptUpdate   = NULL; //SC_SignEncryptUpdate;
	function_list.ST_DecryptVerifyUpdate = NULL; // SC_DecryptVerifyUpdate;
	function_list.ST_GenerateKey         = SC_GenerateKey;
	function_list.ST_GenerateKeyPair     = SC_GenerateKeyPair;
	function_list.ST_WrapKey             = SC_WrapKey;
	function_list.ST_UnwrapKey           = SC_UnwrapKey;
	function_list.ST_DeriveKey           = SC_DeriveKey;
	function_list.ST_SeedRandom          = SC_SeedRandom ;
	function_list.ST_GenerateRandom      = SC_GenerateRandom;
	function_list.ST_GetFunctionStatus   = NULL; // SC_GetFunctionStatus;
	function_list.ST_CancelFunction      = NULL; // SC_CancelFunction;
}
