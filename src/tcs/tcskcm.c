
/*
 * Licensed Materials - Property of IBM
 *
 * trousers - An open source TCG Software Stack
 *
 * (C) Copyright International Business Machines Corp. 2004
 *
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tss/tss.h"
#include "spi_internal_types.h"
#include "tcs_internal_types.h"
#include "tcs_tsp.h"
#include "tcs_utils.h"
#include "tcs_int_literals.h"
#include "capabilities.h"
#include "log.h"
#include "tcsps.h"
#include "req_mgr.h"

TSS_RESULT
TCS_RegisterKey_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
			 TSS_UUID *WrappingKeyUUID,	/* in */
			 TSS_UUID *KeyUUID,	/* in  */
			 UINT32 cKeySize,	/* in */
			 BYTE * rgbKey,	/* in */
			 UINT32 cVendorData,	/* in */
			 BYTE * gbVendorData	/* in */
    )
{
	TSS_RESULT result;
	BOOL is_reg;

	LogDebug1("TCS_RegisterKey");
	if ((result = ctx_verify_context(hContext)))
		return result;

	/*---	Check if parent is registered here */
	if (isUUIDRegistered(WrappingKeyUUID, &is_reg) != TSS_SUCCESS) {
		LogDebug1("Parent not found");
		return TCS_E_FAIL;
	}

	if (is_reg == FALSE) {
		LogDebug1("Wrapping UUID is not registered");
		return TCS_E_KEY_NOT_REGISTERED;
	}

	/*---	Check if key is already regisitered */
	if (isUUIDRegistered(KeyUUID, &is_reg) != TSS_SUCCESS) {
		LogError1("Failed checking if UUID is registered.");
		return TSS_E_INTERNAL_ERROR;
	}

	if (is_reg == TRUE) {
		LogDebug1("UUID is already registered");
		return TCS_E_KEY_ALREADY_REGISTERED;
	}

	/*---	Go ahead and store it in system persistant storage */
	if ((result = writeRegisteredKeyToFile(KeyUUID, WrappingKeyUUID, rgbKey, cKeySize))) {
		LogError1("Error writing key to file");
		return TCS_E_FAIL;
	}

	LogDebug1("Leaving TCS_RegisterKey");
	return TSS_SUCCESS;
}

TSS_RESULT
TCSP_UnregisterKey_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
			    TSS_UUID KeyUUID	/* in  */
    )
{
	TSS_RESULT result;

	if ((result = ctx_verify_context(hContext)))
		return result;

	return removeRegisteredKey(&KeyUUID);
}

TSS_RESULT
TCS_EnumRegisteredKeys_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
				TSS_UUID * pKeyUUID,	/* in    */
				UINT32 * pcKeyHierarchySize,	/* out */
				TSS_KM_KEYINFO ** ppKeyHierarchy	/* out */
    )
{
	TSS_RESULT result = TCS_SUCCESS;
	UINT32 count = 0, i;
	TSS_KM_KEYINFO *ret = NULL;
	TSS_UUID tmp_uuid;
	struct key_disk_cache *disk_ptr, *tmp_ptrs[MAX_KEY_CHILDREN];
	struct key_mem_cache *mem_ptr;
	BOOL is_reg = FALSE;

	LogDebug1("Enum Reg Keys");

	if (pcKeyHierarchySize == NULL || ppKeyHierarchy == NULL)
		return TSS_E_BAD_PARAMETER;

	if ((result = ctx_verify_context(hContext)))
		return result;

	if (pKeyUUID != NULL) {
		/* First have to verify the key is registered */
		if ((result = isUUIDRegistered(pKeyUUID, &is_reg)))
			return result;

		if (is_reg == FALSE) {
			/* This return code is not listed as possible in the TSS 1.1 spec,
			 * but it makes more sense than just TCS_SUCCESS or TCS_E_FAIL */
			return TCS_E_KEY_NOT_REGISTERED;
		}
	}

	/* this entire operation needs to be atomic wrt registered keys. We must
	 * lock the mem cache as well to test if a given key is loaded. */
	pthread_mutex_lock(&disk_cache_lock);
	pthread_mutex_lock(&mem_cache_lock);

	/* return an array of all registered keys if pKeyUUID == NULL */
	if (pKeyUUID == NULL) {
		/*  determine the number of registered keys */
		for (disk_ptr = key_disk_cache_head; disk_ptr; disk_ptr = disk_ptr->next) {
			if (disk_ptr->flags & CACHE_FLAG_VALID)
				count++;
		}

		/* malloc a structure for each of them */
		if (count != 0) {
			ret = getSomeMemory((count * sizeof(TSS_KM_KEYINFO)), hContext);
			if (ret == NULL) {
				LogError1("Malloc Failure");
				count = 0;
				result = TSS_E_OUTOFMEMORY;
				goto done;
			}
		} else {
			goto done;
		}

		/* fill out the structure for each key */
		i = 0;
		for (disk_ptr = key_disk_cache_head; disk_ptr; disk_ptr = disk_ptr->next) {
			if (disk_ptr->flags & CACHE_FLAG_VALID) {
				/* look for a mem cache entry to check if its loaded */
				for (mem_ptr = key_mem_cache_head; mem_ptr; mem_ptr = mem_ptr->next) {
					if (!memcmp(&mem_ptr->uuid, &disk_ptr->uuid, sizeof(TSS_UUID))) {
						if ((result = fill_key_info(disk_ptr, mem_ptr, &ret[i]))) {
							free(ret);
							ret = NULL;
							count = 0;
							goto done;
						}
						break;
					}
				}
				/* if there is no mem cache entry for this key, go ahead and call
				 * fill_key_info(), it will pull everything from disk */
				if (mem_ptr == NULL) {
					if ((result = fill_key_info(disk_ptr, NULL, &ret[i]))) {
						free(ret);
						ret = NULL;
						count = 0;
						goto done;
					}
				}
				i++;
			}
		}
	} else {
		/* return a chain of a key and its parents up to the SRK */
		/*  determine the number of keys in the chain */
		memcpy(&tmp_uuid, pKeyUUID, sizeof(TSS_UUID));
		disk_ptr = key_disk_cache_head;
		while (disk_ptr != NULL && count < MAX_KEY_CHILDREN)
		{
			if (disk_ptr->flags & CACHE_FLAG_VALID &&
				!memcmp(&disk_ptr->uuid, &tmp_uuid, sizeof(TSS_UUID)))
			{
				/* increment count, then search for the parent */
				count++;
				/* save a pointer to this cache entry */
				tmp_ptrs[count - 1] = disk_ptr;
				/* if the parent of this key is NULL, we're at the root of the tree */
				if (!memcmp(&disk_ptr->parent_uuid, &NULL_UUID, sizeof(TSS_UUID)))
					break;
				/* overwrite tmp_uuid with the parent, which we will now search for */
				memcpy(&tmp_uuid, &disk_ptr->parent_uuid, sizeof(TSS_UUID));
				disk_ptr = key_disk_cache_head;
				continue;
			}
			disk_ptr = disk_ptr->next;
		}
		/* when we reach this point, we have an array of TSS_UUID's that leads from the
		 * requested key up to the SRK*/

		/* malloc a structure for each of them */
		if (count != 0) {
			ret = getSomeMemory((count * sizeof(TSS_KM_KEYINFO)), hContext);
			if (ret == NULL) {
				LogError1("Malloc Failure");
				count = 0;
				result = TSS_E_OUTOFMEMORY;
				goto done;
			}
		} else {
			goto done;
		}

		for (i = 0; i < count; i++) {
			/* look for a mem cache entry to check if its loaded */
			for (mem_ptr = key_mem_cache_head; mem_ptr; mem_ptr = mem_ptr->next) {
				if (!memcmp(&mem_ptr->uuid, &tmp_ptrs[i]->uuid, sizeof(TSS_UUID))) {
					if ((result = fill_key_info(tmp_ptrs[i], mem_ptr, &ret[i]))) {
						free(ret);
						ret = NULL;
						count = 0;
						goto done;
					}
					break;
				}
			}
			/* if there is no mem cache entry for this key, go ahead and call
			 * fill_key_info(), it will pull everything from disk */
			if (mem_ptr == NULL) {
				if ((result = fill_key_info(tmp_ptrs[i], NULL, &ret[i]))) {
					free(ret);
					ret = NULL;
					count = 0;
					goto done;
				}
			}
		}

	}
done:

	pthread_mutex_unlock(&disk_cache_lock);
	pthread_mutex_unlock(&mem_cache_lock);

	*ppKeyHierarchy = ret;
	*pcKeyHierarchySize = count;

	return result;
}

TSS_RESULT
TCS_GetRegisteredKey_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
			      TSS_UUID *KeyUUID,	/* in */
			      TSS_KM_KEYINFO ** ppKeyInfo	/* out */
    )
{
	TSS_RESULT result;
	UINT16 offset;
	BYTE tcpaKeyBlob[1024];
	TCPA_KEY tcpaKey;
	UINT16 keySize = sizeof (tcpaKeyBlob);
	TSS_UUID *parentUUID;

	/* This should be set in case we return before the malloc */
	*ppKeyInfo = NULL;

	if ((result = ctx_verify_context(hContext)))
		return result;

	if ((result = getRegisteredKeyByUUID(KeyUUID, tcpaKeyBlob, &keySize))) {
		return TCS_E_KEY_NOT_REGISTERED;
	}

	if ((result = getParentUUIDByUUID(KeyUUID, &parentUUID)))
		return TCS_E_FAIL;

	*ppKeyInfo = malloc(sizeof(TSS_KM_KEYINFO));
	if (*ppKeyInfo == NULL) {
		LogError1("Malloc Failure.");
		return TSS_E_OUTOFMEMORY;
	}

	offset = 0;
	UnloadBlob_KEY(&offset, tcpaKeyBlob, &tcpaKey);

	(*ppKeyInfo)->bAuthDataUsage = tcpaKey.authDataUsage;

	(*ppKeyInfo)->fIsLoaded = FALSE;

	(*ppKeyInfo)->versionInfo.bMajor = tcpaKey.ver.major;
	(*ppKeyInfo)->versionInfo.bMinor = tcpaKey.ver.minor;
	(*ppKeyInfo)->versionInfo.bRevMajor = tcpaKey.ver.revMajor;
	(*ppKeyInfo)->versionInfo.bRevMinor = tcpaKey.ver.revMinor;

	memcpy(&((*ppKeyInfo)->keyUUID), KeyUUID, sizeof(TSS_UUID));

	(*ppKeyInfo)->ulVendorDataLength = 0;
	(*ppKeyInfo)->rgbVendorData = 0;

	memcpy(&((*ppKeyInfo)->parentKeyUUID), parentUUID, sizeof(TSS_UUID));
	return TSS_SUCCESS;
}

TSS_RESULT
TCS_GetRegisteredKeyBlob_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
				  TSS_UUID *KeyUUID,	/* in */
				  UINT32 * pcKeySize,	/* out */
				  BYTE ** prgbKey	/* out */
    )
{
	UINT16 keySize;
	BYTE buffer[1024];
	TSS_RESULT result;

	if ((result = ctx_verify_context(hContext)))
		return result;

	keySize = sizeof (buffer);
	if ((result = getRegisteredKeyByUUID(KeyUUID, buffer, &keySize)))
		return TCS_E_KEY_NOT_REGISTERED;

	*prgbKey = getSomeMemory(keySize, hContext);
	if (*prgbKey == NULL) {
		LogError("malloc of %d bytes failed.", keySize);
		return TSS_E_OUTOFMEMORY;
	} else {
		memcpy(*prgbKey, buffer, keySize);
	}
	*pcKeySize = keySize;

	return TSS_SUCCESS;
}

TSS_RESULT
TCSP_LoadKeyByBlob_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
			    TCS_KEY_HANDLE hUnwrappingKey,	/* in */
			    UINT32 cWrappedKeyBlobSize,	/* in */
			    BYTE * rgbWrappedKeyBlob,	/* in */
			    TCS_AUTH * pAuth,	/* in, out */
			    TCS_KEY_HANDLE * phKeyTCSI,	/* out */
			    TCS_KEY_HANDLE * phKeyHMAC	/* out */
    )
{
	UINT16 offset;
	TSS_RESULT result;
	UINT32 paramSize;
	TCPA_KEY *key = NULL;
	TCPA_KEY_HANDLE myKeySlot;
	TCS_KEY_HANDLE myTcsKeyHandle;
	TCPA_STORE_PUBKEY *myPubKey;
	TCPA_STORE_PUBKEY *parentPubKey = NULL;
	TCPA_KEY_HANDLE parentKeySlot;
	BOOL needToSendPacket = TRUE, canLoad;
	BYTE txBlob[TPM_TXBLOB_SIZE];

	cWrappedKeyBlobSize = 0;

	do {
		if ((result = ctx_verify_context(hContext)))
			break;

		if (pAuth != NULL) {
			LogDebug1("Auth Used");
			if ((result = auth_mgr_check(hContext, pAuth->AuthHandle)))
				break;
		} else {
			LogDebug1("No Auth Used");
		}

		key = malloc(sizeof(TCPA_KEY));
		if (key == NULL) {
			LogError("malloc of %d bytes failed.", sizeof(TCPA_KEY));
			return TSS_E_OUTOFMEMORY;
		}

		offset = 0;
		if ((result = UnloadBlob_KEY(&offset, rgbWrappedKeyBlob, key))) {
			free(key);
			return result;
		}
		cWrappedKeyBlobSize = offset;

		/******************************************
		 *	The first thing to make sure is that the parent is loaded.
		 *	If the parentKeySlot is invalid, then it either wasn't found in the cache
		 *	or it was evicted.  Then checking if it was ever in the cache by calling
		 *	getParentPubByPub will tell us whether or not there is an error.  If this
		 *	unregistered parent was never loaded by the user, then he's hosed and
		 *	this is an error.  If there is knowledge, then the shim is called to load
		 *	the parent by it's public key.
		 *********************************************/

		/* Check the mem cache to see if there is a TPM handle associated with the
		 * parent's TCS handle */
		if ((parentKeySlot = getSlotByHandle(hUnwrappingKey)) == NULL_TPM_HANDLE) {
			parentPubKey = getPubBySlot(hUnwrappingKey);
			if (parentPubKey == NULL) {
				result = TCS_E_KM_LOADFAILED;
				break;
			}
			/* Otherwise, try to load it using the shim */
			if ((result = LoadKeyShim(hContext, parentPubKey, NULL, &parentKeySlot)))
				break;
		}
		/*******************************************
		 *Call LoadKeyShim
		 *If it passes, we had prior knowledge of this key and we can avoid redundant copies of it
		 *******************************************/

		/*---	If it's an authorized load, then assume that we brute-force load it every time */
		if (pAuth == NULL) {
			LogDebug1("Checking if LoadKeyByBlob can be avoided by using existing key");

			myTcsKeyHandle = getTCSKeyHandleByPub(&key->pubKey);
			if (myTcsKeyHandle != NULL_TCS_HANDLE) {
				LogDebug1("tcs key handle exists");

				myKeySlot = getSlotByHandle(myTcsKeyHandle);
				if (myKeySlot != NULL_TPM_HANDLE && isKeyLoaded(myKeySlot) == TRUE) {
					needToSendPacket = FALSE;
					LogDebug1("Don't need to reload this key.");
					result = TSS_SUCCESS;
					break;

				}
			}

		}

		/******************************************
		 *Now we just have to check if there is enough room in the chip.
		 *********************************************/

		if ((result = canILoadThisKey(&key->algorithmParms, &canLoad))) {
			destroy_key_refs(key);
			free(key);
			return result;
		}

		while (canLoad == FALSE) {
			/* Evict a key that isn't the parent */
			if ((result = evictFirstKey(hUnwrappingKey))) {
				destroy_key_refs(key);
				free(key);
				return result;
			}

			if ((result = canILoadThisKey(&key->algorithmParms, &canLoad))) {
				destroy_key_refs(key);
				free(key);
				return result;
			}
		}

		LogDebug1("Entering LoadKey by blob");

		/****************************************
		 *	Now the parent is loaded and all of the info is ready.
		 *	Send the loadkey command.  If the auth is a NULL Pointer
		 *	then this represents a NoAuth load
		 ********************************************/

		offset = 10;
		LoadBlob_UINT32(&offset, parentKeySlot, txBlob, "parentHandle");
		LoadBlob(&offset, cWrappedKeyBlobSize, txBlob, rgbWrappedKeyBlob, "wrapped blob");
		if (pAuth != NULL) {
			LoadBlob_Auth(&offset, txBlob, pAuth);
			LoadBlob_Header(TPM_TAG_RQU_AUTH1_COMMAND, offset, TPM_ORD_LoadKey, txBlob);
		} else
			LoadBlob_Header(TPM_TAG_RQU_COMMAND, offset, TPM_ORD_LoadKey, txBlob);

		if ((result = req_mgr_submit_req(txBlob)))
			break;
	} while (0);

	if (result) {
		/*---	Should never get in here on a needToSendPacket == FALSE path */
		if (pAuth != NULL)
			auth_mgr_release_auth(pAuth->AuthHandle);
		if (key) {
			destroy_key_refs(key);
			free(key);
		}
		return result;
	}

	if (needToSendPacket == TRUE) {
		if ((result = UnloadBlob_Header(txBlob, &paramSize))) {
			destroy_key_refs(key);
			free(key);
			return result;
		}
		offset = 10;
		/*---	Finish unloading the stuff */
		UnloadBlob_UINT32(&offset, &myKeySlot, txBlob, "handle back");
		if (pAuth != NULL) {
			UnloadBlob_Auth(&offset, txBlob, pAuth);
			if (pAuth->fContinueAuthSession == FALSE)
				auth_mgr_release_auth(pAuth->AuthHandle);

		}
	} else {
		LogData("Key slot is", myKeySlot);
	}

	/*---	Get the keyInfo from the key for storage */
	myPubKey = &key->pubKey;
	/***************************************
	 *See if a TCSKeyHandle already exists.
	 *	If it's 0, then it doesn't exist, and we need new knowledge of the key.
	 *	If it exists, then just register the new keySlot with that existing handle
	 *****************************************/

	myTcsKeyHandle = getTCSKeyHandleByPub(&key->pubKey);
	if (myTcsKeyHandle == NULL_TCS_HANDLE) {
		LogDebug1("No existing key handle for this key, need to create a new one");
		/*---	Get a new TCS Key Handle */
		myTcsKeyHandle = getNextTcsKeyHandle();
		/*if it was an authorized load, then we can't make complete knowledge about it */

		/*---	Add this info to the memory cache */
		if (pAuth == NULL) {
			offset = 0;
			destroy_key_refs(key);
			if ((result = UnloadBlob_KEY(&offset, rgbWrappedKeyBlob, key)))
				return result;
		}

		result = add_mem_cache_entry(myTcsKeyHandle, myKeySlot, key);
		if (result != TSS_SUCCESS) {
			destroy_key_refs(key);
			free(key);
			return result;
		}

		if (ctx_mark_key_loaded(hContext, myTcsKeyHandle)) {
			LogError1("Error marking key as loaded");
			destroy_key_refs(key);
			free(key);
			return TSS_E_INTERNAL_ERROR;
		}

		if (pAuth == NULL) {
			result = setParentByHandle(myTcsKeyHandle, hUnwrappingKey);
			if (result != TSS_SUCCESS) {
				LogError1("setParentBlobByHandle failed.");
				destroy_key_refs(key);
				free(key);
				return result;
			}
		}

	} else
		setSlotByHandle(myTcsKeyHandle, myKeySlot);

	/*---	Setup the outHandles */
	*phKeyTCSI = myTcsKeyHandle;
	*phKeyHMAC = myKeySlot;

	LogDebug("Key handles for loadKeyByBlob slot:%.8X tcshandle:%.8X", myKeySlot, myTcsKeyHandle);
	LogResult("LoadKey By Blob", result);

	destroy_key_refs(key);
	free(key);

	return TSS_SUCCESS;
}

TSS_RESULT
TCSP_LoadKeyByUUID_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
			    TSS_UUID *KeyUUID,	/* in */
			    TCS_LOADKEY_INFO * pLoadKeyInfo,	/* in, out */
			    TCS_KEY_HANDLE * phKeyTCSI	/* out */
    )
{
	UINT32 keyslot;
	TSS_RESULT result;
	UINT16 offset;
	TSS_UUID *parentUuid;
	BYTE keyBlob[0x1000];
	UINT16 keySize = sizeof (keyBlob);
	TCPA_KEY myKey;
	TCPA_STORE_PUBKEY *parentPub;
	TCPA_KEY_HANDLE parentKeySlot;
	TCS_KEY_HANDLE parentTCSKeyHandle;
	TCS_LOADKEY_INFO tmpInfo;

	LogDebug1("LoadKeyByUUID");
	if ((result = ctx_verify_context(hContext)))
		return result;

	/*********************************************************************
	 *	The first thing to do in this func is setup all the info and make sure
	 *		that we get it all from either the keyfile or the keyCache
	 *		also, it's important to return if the key is already loaded
	 ***********************************************************************/

	if (getRegisteredKeyByUUID(KeyUUID, keyBlob, &keySize))
		return TCS_E_KEY_NOT_REGISTERED;

	/*---	Now that there is a key, unload it for later use */
	offset = 0;
	UnloadBlob_KEY(&offset, keyBlob, &myKey);

	/*---	First, check if it's actually loaded now or was previously loaded */
	*phKeyTCSI = getTCSKeyHandleByPub(&myKey.pubKey);
	LogData("TCSKeyHandle is", *phKeyTCSI);
	destroy_key_refs(&myKey);

	if (*phKeyTCSI != NULL_TCS_HANDLE) {
		/* The key was at least previously loaded, now check if its loaded now */
		if (getSlotByHandle(*phKeyTCSI) != NULL_TPM_HANDLE) {
			if (ctx_mark_key_loaded(hContext, *phKeyTCSI)) {
				LogError1("Error marking key as loaded");
				return TSS_E_INTERNAL_ERROR;
			}
			/* its loaded now */
			return TSS_SUCCESS;
		}
	}

	/*---	Get my parent's UUID.  Since My key is registered, my parent should be as well. */
	if ((result = getParentUUIDByUUID(KeyUUID, &parentUuid)))
		return TCS_E_KM_LOADFAILED;

	/*---	Get the parentPublic key from the mem cache.
	 * If this is NULL, the parent isn't loaded yet
	 */
	parentPub = getPubByUuid(parentUuid);

	/*********************************************************************
	 *	At this point the parent should be loaded.  If the parent hasn't been loaded
	 *		during this cycle, then it will not be in the cache knowledge.  If it is
	 *		in the cache knowledge, then it has been loaded at some point, but there
	 *		is no guarantee that it is still loaded so the shim should be called.
	 ***********************************************************************/

	/*---	If no parentPublic information is in the cache, then need to load the parent. */
	if (parentPub == NULL) {
		/*---	Load the parent by it's UUID */
		if ((result = TCSP_LoadKeyByUUID_Internal(hContext, parentUuid,
					NULL, &parentTCSKeyHandle)))
			return result;
	}
	/*---	Parent is already loaded, or was loaded at some time */
	else {
		if ((result = LoadKeyShim(hContext, parentPub, parentUuid, &parentKeySlot)))
			return result;
		parentTCSKeyHandle = getAnyHandleBySlot(parentKeySlot);
	}
	/*******************************************************
	 * If no errors have happend up till now, then the parent is loaded and ready for use.
	 * The parent's TCS Handle should be in parentTCSKeyHandle.
	 ******************************************************/
	return TCSP_LoadKeyByBlob_Internal(hContext,
			parentTCSKeyHandle,
			keySize, keyBlob, NULL, phKeyTCSI, &keyslot);
}

TSS_RESULT
TCSP_EvictKey_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
		       TCS_KEY_HANDLE hKey	/* in */
    )
{
	TSS_RESULT result;
	TCPA_KEY_HANDLE tpm_handle;

	if ((result = ctx_verify_context(hContext)))
		return result;

	tpm_handle = getSlotByHandle(hKey);
	if (tpm_handle == NULL_TPM_HANDLE)
		return TSS_SUCCESS;	/*let's call this success if the key is already evicted */

	if ((result = internal_EvictByKeySlot(tpm_handle)))
		return result;

	result = setSlotBySlot(tpm_handle, NULL_TPM_HANDLE);

	return result;
}

TSS_RESULT
TCSP_CreateWrapKey_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
			    TCS_KEY_HANDLE hWrappingKey,	/* in */
			    TCPA_ENCAUTH KeyUsageAuth,	/* in */
			    TCPA_ENCAUTH KeyMigrationAuth,	/* in */
			    UINT32 keyInfoSize,	/* in */
			    BYTE * keyInfo,	/* in */
			    UINT32 * keyDataSize,	/* out */
			    BYTE ** keyData,	/* out */
			    TCS_AUTH * pAuth	/* in, out */
    )
{
	UINT16 offset;
	UINT32 paramSize;
	TSS_RESULT result;
	TCPA_KEY keyContainer;
	TCPA_KEY_HANDLE parentSlot;
	BYTE txBlob[TPM_TXBLOB_SIZE];

	LogDebug1("Entering Create Wrap Key");

	do {
		if ((result = ctx_verify_context(hContext)))
			break;

		if ((result = auth_mgr_check(hContext, pAuth->AuthHandle)))
			break;

		/* Since hWrappingKey must already be loaded, we can fail immediately if
		 * getSlotByHandle() fails.*/
		parentSlot = getSlotByHandle_lock(hWrappingKey);
		if (parentSlot == NULL_TPM_HANDLE) {
			result = TCS_E_FAIL;
			break;
		}

		offset = 10;
		LoadBlob_UINT32(&offset, parentSlot, txBlob, "parent handle");
		LoadBlob(&offset, TPM_ENCAUTH_SIZE, txBlob, KeyUsageAuth.encauth, "data usage encauth");
		LoadBlob(&offset, TPM_ENCAUTH_SIZE, txBlob,
			 KeyMigrationAuth.encauth, "data mig encauth");
		LoadBlob(&offset, keyInfoSize, txBlob, keyInfo, "Key");
		LoadBlob_Auth(&offset, txBlob, pAuth);
		LoadBlob_Header(TPM_TAG_RQU_AUTH1_COMMAND, offset,
				TPM_ORD_CreateWrapKey, txBlob);
		if ((result = req_mgr_submit_req(txBlob)))
			break;

	} while (0);
	if (result) {
		auth_mgr_release_auth(pAuth->AuthHandle);
		return result;
	}

	offset = 10;
	result = UnloadBlob_Header(txBlob, &paramSize);

	if (pAuth->fContinueAuthSession == FALSE)
		auth_mgr_release_auth(pAuth->AuthHandle);

	if (!result) {
		/*===	First get the data from the packet */
		UnloadBlob_KEY(&offset, txBlob, &keyContainer);
		/*===	Here's how big it is */
		*keyDataSize = offset - 10;
		/*===	malloc the outBuffer */
		*keyData = getSomeMemory(*keyDataSize, hContext);
		if (*keyData == NULL) {
			LogError("malloc of %d bytes failed.", *keyDataSize);
			result = TSS_E_OUTOFMEMORY;
		} else {
			/*===	Reset the offset and load it into the outbuf */
			memcpy(*keyData, &txBlob[10], *keyDataSize);
		}
/*		offset = 10; */
/*		LoadBlob_KEY( &offset, *prgbKey, &keyContainer ); */
		/*===	offset is now restored...continue */
/*		if( pAuth != NULL ) */

		UnloadBlob_Auth(&offset, txBlob, pAuth);
		if (result)
			auth_mgr_release_auth(pAuth->AuthHandle);

		destroy_key_refs(&keyContainer);
	}
	LogResult("Create Wrap Key", result);

	return result;
}

TSS_RESULT
TCSP_GetPubKey_Internal(TCS_CONTEXT_HANDLE hContext,	/* in */
			TCS_KEY_HANDLE hKey,	/* in */
			TCS_AUTH * pAuth,	/* in, out */
			UINT32 * pcPubKeySize,	/* out */
			BYTE ** prgbPubKey	/* out */
    )
{
	UINT16 offset;
	UINT32 paramSize;
	TSS_RESULT result;
	TCPA_PUBKEY pubContainer;
	TCPA_KEY_HANDLE keySlot;
	BYTE srkKeyBlob[1024];
	UINT16 srkKeySize;
	TCPA_KEY srkKey;
	UINT32 rc;
	TCPA_VERSION *version;
	BYTE txBlob[TPM_TXBLOB_SIZE];

	LogDebug1("Entering Get pub key");
	do {
		if ((result = ctx_verify_context(hContext)))
			break;

		if (pAuth != NULL) {
			LogDebug1("Auth Used");
			if ((result = auth_mgr_check(hContext, pAuth->AuthHandle)))
				break;
		} else {
			LogDebug1("No Auth");
		}

		if (ensureKeyIsLoaded(hContext, hKey, &keySlot)) {
			result = TCS_E_KM_LOADFAILED;
			break;
		}
#if 0
		keySlot = getSlotByHandle_lock(hKey);
		if (keySlot == NULL_TPM_HANDLE) {
			result = TCS_E_KM_LOADFAILED;
			break;
		}
#endif
		offset = 10;
		LoadBlob_UINT32(&offset, keySlot, txBlob, "key handle");
		if (pAuth != NULL) {
			LoadBlob_Auth(&offset, txBlob, pAuth);
			LoadBlob_Header(TPM_TAG_RQU_AUTH1_COMMAND, offset, TPM_ORD_GetPubKey, txBlob);
		} else {
			LoadBlob_Header(TPM_TAG_RQU_COMMAND, offset, TPM_ORD_GetPubKey, txBlob);
		}

		if ((result = req_mgr_submit_req(txBlob)))
			break;
	} while (0);
	if (result) {
		if (pAuth != NULL)
			auth_mgr_release_auth(pAuth->AuthHandle);
		return result;
	}

	offset = 10;
	result = UnloadBlob_Header(txBlob, &paramSize);

	if (pAuth && pAuth->fContinueAuthSession == FALSE)
		auth_mgr_release_auth(pAuth->AuthHandle);

	if (!result) {
		UnloadBlob_PUBKEY(&offset, txBlob, &pubContainer);
		*pcPubKeySize = offset - 10;
		*prgbPubKey = getSomeMemory(*pcPubKeySize, hContext);
		if (*prgbPubKey == NULL) {
			LogError("malloc of %d bytes failed.", *pcPubKeySize);
			result = TSS_E_OUTOFMEMORY;
		} else {
			memcpy(*prgbPubKey, &txBlob[10], *pcPubKeySize);
		}

		if (pAuth != NULL) {
			UnloadBlob_Auth(&offset, txBlob, pAuth);
			if (result)
				auth_mgr_release_auth(pAuth->AuthHandle);
		}
#if 0
		if (keySlot == SRK_TPM_HANDLE) {
			/*---	If it's the SRK, make sure the key storage isn't stale */
			LogDebug1("Checking SRK in storage");
			srkKeySize = sizeof (srkKeyBlob);
			rc = getRegisteredKeyByUUID(&SRK_UUID, srkKeyBlob, &srkKeySize);
			if (rc) {
				LogDebug1("SRK isn't in storage, setting it.  Have to guess at some parms");
				memset(&srkKey, 0x00, sizeof (TCPA_KEY));

				srkKey.pubKey.keyLength = 0x100;
				srkKey.pubKey.key = malloc(0x100);
				if (srkKey.pubKey.key == NULL) {
					LogError1("Malloc Failure.");
					return TSS_E_OUTOFMEMORY;
				}
				memcpy(srkKey.pubKey.key, pubContainer.pubKey.key, 0x100);

				srkKey.algorithmParms.algorithmID = pubContainer.algorithmParms.algorithmID;
				srkKey.algorithmParms.parmSize = pubContainer.algorithmParms.parmSize;
				srkKey.algorithmParms.parms = malloc(pubContainer.algorithmParms.parmSize);
				if (srkKey.algorithmParms.parms == NULL) {
					LogError1("Malloc Failure.");
					return TSS_E_OUTOFMEMORY;
				}
				memcpy(srkKey.algorithmParms.parms,
				       pubContainer.algorithmParms.parms,
				       pubContainer.algorithmParms.parmSize);
				srkKey.algorithmParms.encScheme = pubContainer.algorithmParms.encScheme;
				srkKey.algorithmParms.sigScheme = pubContainer.algorithmParms.sigScheme;

				srkKey.authDataUsage = 0x00;

				srkKey.keyUsage = 0x11;

				version = getCurrentVersion();
				if (version == NULL)
					return TCS_E_FAIL;

				memcpy(&srkKey.ver, version, sizeof (TCPA_VERSION));
				offset = 0;
				LoadBlob_KEY(&offset, srkKeyBlob, &srkKey);
				srkKeySize = offset;

				free(srkKey.algorithmParms.parms);

				if ((result = writeRegisteredKeyToFile(&SRK_UUID, &NULL_UUID, srkKeyBlob, srkKeySize))) {
					LogError1("Error writing SRK to disk");
					return result;
				}
			} else {
				offset = 0;
				UnloadBlob_KEY(&offset, srkKeyBlob, &srkKey);
				if (srkKey.pubKey.keyLength == pubContainer.pubKey.keyLength
				    && srkKey.pubKey.key != NULL
				    && pubContainer.pubKey.key != NULL
				    && !memcmp(srkKey.pubKey.key, pubContainer.pubKey.key, 20)) {
					/*this is good */
					LogDebug1("SRK in storage is the same");
				} else {
					LogDebug1("SRK in storage is different.  Resetting storage");
					removeRegisteredKeyFromFile(NULL);

					if (srkKey.pubKey.key == NULL) {
						srkKey.pubKey.key = malloc(0x100);
						if (srkKey.pubKey.key == NULL) {
							LogError1("Malloc Failure.");
							return TSS_E_OUTOFMEMORY;
						}
					}

					memcpy(srkKey.pubKey.key, *prgbPubKey, 0x100);
					offset = 0;
					LoadBlob_KEY(&offset, srkKeyBlob, &srkKey);
					srkKeySize = offset;

					if ((result = writeRegisteredKeyToFile(&SRK_UUID, &NULL_UUID, srkKeyBlob, srkKeySize))) {
						LogError1("Error writing SRK to disk");
						return result;
					}
#if 0
					/*---	Need to update the cache as well */
					CacheInit = 0;
					result = initCache();
#endif
				}
			}
		}
#endif
	}
/*	AppendAudit(0, TPM_ORD_GetPubKey, result); */
	LogResult("Get Public Key", result);
	return result;
}

TSS_RESULT
TCSP_MakeIdentity_Internal(TCS_CONTEXT_HANDLE hContext,	/* in  */
			   TCPA_ENCAUTH identityAuth,	/* in */
			   TCPA_CHOSENID_HASH IDLabel_PrivCAHash,	/* in */
			   UINT32 idKeyInfoSize,	/*in */
			   BYTE * idKeyInfo,	/*in */
			   TCS_AUTH * pSrkAuth,	/* in, out */
			   TCS_AUTH * pOwnerAuth,	/* in, out */
			   UINT32 * idKeySize,	/* out */
			   BYTE ** idKey,	/* out */
			   UINT32 * pcIdentityBindingSize,	/* out */
			   BYTE ** prgbIdentityBinding,	/* out */
			   UINT32 * pcEndorsementCredentialSize,	/* out */
			   BYTE ** prgbEndorsementCredential,	/* out */
			   UINT32 * pcPlatformCredentialSize,	/* out */
			   BYTE ** prgbPlatformCredential,	/* out */
			   UINT32 * pcConformanceCredentialSize,	/* out */
			   BYTE ** prgbConformanceCredential	/* out */
    )
{
	UINT16 offset;
	UINT32 paramSize;
	TSS_RESULT result;
	/*    TCPA_DIGEST digest; */
	/*    BYTE hashBlob[512]; */
	TCPA_KEY idKeyContainer;
	BYTE txBlob[TPM_TXBLOB_SIZE];

	LogDebug1("Entering makeidentity");
	LogDebug1("SM DEBUG <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< @TCS");
	do {
		if ((result = ctx_verify_context(hContext)))
			break;

		if (pSrkAuth != NULL) {
			LogDebug1("Auth Used");
			if ((result = auth_mgr_check(hContext, pSrkAuth->AuthHandle)))
				break;
		} else {
			LogDebug1("No Auth");
		}

		if ((result = auth_mgr_check(hContext, pOwnerAuth->AuthHandle)))
			break;

		offset = 0;
		/*                LoadBlob( &offset, cLabelSize, hashBlob, rgbLabel, NULL ); */
		/*                LoadBlob( &offset, cPrivacyCAPubKeySize, hashBlob, rgbPrivacyCAPubKey, NULL ); */

		LogDebug1("Now buidling Parm block\n");
		offset = 10;
		/*LoadBlob( &offset, idKeyInfoSize, txBlob, idKeyInfo, "idKeyInfo" );  */
		LoadBlob(&offset, TPM_ENCAUTH_SIZE, txBlob, identityAuth.encauth, "encAuth");
		/*LoadBlob_UINT32( &offset, 20, txBlob, "label size"); */
		LoadBlob(&offset, 20, txBlob, IDLabel_PrivCAHash.digest, "label");
		LoadBlob(&offset, idKeyInfoSize, txBlob, idKeyInfo, "idKeyInfo");
		if (pSrkAuth != NULL) {
			LoadBlob_Auth(&offset, txBlob, pSrkAuth);
			LoadBlob_Auth(&offset, txBlob, pOwnerAuth);
			LoadBlob_Header(TPM_TAG_RQU_AUTH2_COMMAND, offset, TPM_ORD_MakeIdentity, txBlob);
		} else {
			LoadBlob_Auth(&offset, txBlob, pOwnerAuth);
			LoadBlob_Header(TPM_TAG_RQU_AUTH1_COMMAND, offset, TPM_ORD_MakeIdentity, txBlob);
		}

		if ((result = req_mgr_submit_req(txBlob)))
			break;
	} while (0);

	if (result) {
		if (pSrkAuth != NULL)
			auth_mgr_release_auth(pSrkAuth->AuthHandle);
		auth_mgr_release_auth(pOwnerAuth->AuthHandle);
		return result;
	}

	offset = 10;
	result = UnloadBlob_Header(txBlob, &paramSize);

	if (pSrkAuth && pSrkAuth->fContinueAuthSession == FALSE)
		auth_mgr_release_auth(pSrkAuth->AuthHandle);

	if (pOwnerAuth->fContinueAuthSession == FALSE)
		auth_mgr_release_auth(pOwnerAuth->AuthHandle);

	if (!result) {
		UnloadBlob_KEY(&offset, txBlob, &idKeyContainer);
		*idKeySize = offset - 10;
		*idKey = getSomeMemory(*idKeySize, hContext);
		if (*idKey == NULL) {
			LogError("malloc of %d bytes failed.", *idKeySize);
			result = TSS_E_OUTOFMEMORY;
		} else {
			/*LogResult( "idKey size",*idKeySize); */
			memcpy(*idKey, &txBlob[10], *idKeySize);
		}

		UnloadBlob_UINT32(&offset, pcIdentityBindingSize, txBlob, "bind size");
		*prgbIdentityBinding = getSomeMemory(*pcIdentityBindingSize, hContext);
		if (*prgbIdentityBinding == NULL) {
			LogError("malloc of %d bytes failed.", *pcIdentityBindingSize);
			result = TSS_E_OUTOFMEMORY;
		} else {
			UnloadBlob(&offset, *pcIdentityBindingSize, txBlob,
					*prgbIdentityBinding, "id bind");
		}
		/* RCC */
		*pcEndorsementCredentialSize = 0;
		*pcPlatformCredentialSize = 0;
		*pcConformanceCredentialSize = 0;

		if (pSrkAuth != NULL) {
			UnloadBlob_Auth(&offset, txBlob, pSrkAuth);
			if (result)
				auth_mgr_release_auth(pSrkAuth->AuthHandle);
		}
		UnloadBlob_Auth(&offset, txBlob, pOwnerAuth);
		if (result)
			auth_mgr_release_auth(pOwnerAuth->AuthHandle);
	}
	/*    AppendAudit(0, TPM_ORD_MakeIdentity, result); */
	LogResult("Make Identity", result);
	return result;
}

TSS_RESULT
TCSP_GetRegisteredKeyByPublicInfo_Internal(TCS_CONTEXT_HANDLE tcsContext, TCPA_ALGORITHM_ID algID,	/* in */
					   UINT32 ulPublicInfoLength,	/* in */
					   BYTE * rgbPublicInfo,	/* in */
					   UINT32 * keySize, BYTE ** keyBlob)
{
	TCPA_STORE_PUBKEY pubKey;
	UINT16 keyContainerSize;
	BYTE keyContainer[1024];
	TSS_RESULT result = TSS_SUCCESS;
	TSS_UUID *uuid;

	if ((result = ctx_verify_context(tcsContext)))
		return result;

	if (algID == TCPA_ALG_RSA) {
		/*---	Convert Public info to a structure */
		pubKey.keyLength = ulPublicInfoLength;
		pubKey.key = malloc(pubKey.keyLength);
		if (pubKey.key == NULL) {
			LogError("malloc of %d bytes failed.", pubKey.keyLength);
			return TSS_E_OUTOFMEMORY;
		} else {
			memcpy(pubKey.key, rgbPublicInfo, pubKey.keyLength);
		}

		/*---	Get the UUID from the Registered File */
		result = getRegisteredUuidByPub(&pubKey, &uuid);
		free(pubKey.key);
		if (result)
			return TCS_E_KEY_NOT_REGISTERED;

		/*---	Use the UUID to get the Key */
		keyContainerSize = sizeof (keyContainer);
		if ((result = getRegisteredKeyByUUID(uuid, keyContainer, &keyContainerSize)))
			return TCS_E_KEY_NOT_REGISTERED;

		/*--	Put it in the output parm's */
		*keySize = keyContainerSize;
		*keyBlob = getSomeMemory(*keySize, tcsContext);
		if (*keyBlob == NULL) {
			LogError("malloc of %d bytes failed.", *keySize);
			result = TSS_E_OUTOFMEMORY;
		} else {
			memcpy(*keyBlob, keyContainer, *keySize);
		}
	} else {
		return TCS_E_FAIL;	/*don't know how to support yet */
	}

	return result;
}
