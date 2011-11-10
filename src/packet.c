/*
 *  packet.c
 *  libsimplepgp
 *
 *  Created by Trevor Bentley on 11/1/11.
 *  Copyright 2011 Trevor Bentley. All rights reserved.
 *
 */

#include "packet.h"
#include <stdio.h>
#include <setjmp.h>
#include "gcrypt.h"
#include <wchar.h>
#include <locale.h>
#include "keychain.h"
#include <string.h>


/**********************************************************************
**
** MACROS
**
***********************************************************************/
#pragma mark Macros

#define RAISE(err) do { \
		_spgp_err = (err); \
    LOG_PRINT("raise 0x%X\n",_spgp_err); \
    longjmp(exception,_spgp_err); \
  } while(0)

#define SAFE_IDX_INCREMENT(idx,max) \
	do{ \
		if (++(idx)>=(max)) {\
  		RAISE(BUFFER_OVERFLOW);\
    } \
  } while(0)


/**********************************************************************
**
** Static variables
**
***********************************************************************/

static uint32_t _spgp_err;
static jmp_buf exception;

#ifdef DEBUG_LOG_ENABLED
uint8_t debug_log_enabled = 1;
#else
uint8_t debug_log_enabled = 0;
#endif


/**********************************************************************
**
** Static function prototypes
**
***********************************************************************/

static uint8_t spgp_parse_header(uint8_t *msg, uint32_t *idx, 
														uint32_t length, spgp_packet_t *pkt);

static uint32_t spgp_new_header_length(uint8_t *header, 
																			uint8_t *header_len,
                                      uint8_t *is_partial);

static uint8_t spgp_parse_user_id(uint8_t *msg, uint32_t *idx, 
          												uint32_t length, spgp_packet_t *pkt);
                                      
static uint8_t spgp_generate_fingerprint(spgp_packet_t *pkt);
                               
static uint8_t spgp_verify_decrypted_data(uint8_t *data, uint32_t length);

static uint8_t spgp_generate_cipher_key(spgp_packet_t *pkt,
																			  uint8_t *passphrase, uint32_t length);

static uint8_t spgp_parse_public_key(uint8_t *msg, uint32_t *idx, 
          													 uint32_t length, spgp_packet_t *pkt);
                                     
static uint8_t spgp_parse_secret_key(uint8_t *msg, uint32_t *idx, 
          													 uint32_t length, spgp_packet_t *pkt);
                
static spgp_packet_t *spgp_next_secret_key_packet(spgp_packet_t *msg);
                
static uint8_t spgp_decrypt_secret_key(spgp_packet_t *pkt, 
                                			 uint8_t *passphrase, uint32_t length);
                              
static uint8_t spgp_parse_encrypted_packet(uint8_t *msg, 
                                           uint32_t *idx, 
          														 		 uint32_t length, 
                                           spgp_packet_t *pkt);
                                 
static spgp_packet_t *spgp_find_session_packet(spgp_packet_t *chain);
         
static uint8_t spgp_parse_session_packet(uint8_t *msg, uint32_t *idx, 
          													 		 uint32_t length, spgp_packet_t *pkt);
                               
static spgp_packet_t *spgp_secret_key_matching_id(spgp_packet_t *chain,
																									uint8_t *keyid);
                                         
static uint32_t spgp_mpi_length(uint8_t *mpi);
                                                
static spgp_mpi_t *spgp_read_mpi(uint8_t *msg, uint32_t *idx,
														 uint32_t length);
                             
static uint8_t spgp_read_all_public_mpis(uint8_t *msg, 
                                         uint32_t *idx,
														 						 uint32_t length, 
                                         spgp_public_pkt_t *pub);
                                         
static uint8_t spgp_read_all_secret_mpis(uint8_t *msg, 
                                         uint32_t *idx,
														 						 uint32_t length, 
                                         spgp_secret_pkt_t *secret);
                                         
static uint8_t spgp_read_salt(uint8_t *msg, 
                              uint32_t *idx,
                              uint32_t length, 
                              spgp_secret_pkt_t *secret);
                              
static uint8_t spgp_read_iv(uint8_t *msg, 
                            uint32_t *idx,
                            uint32_t length, 
                            spgp_secret_pkt_t *secret);
                            
static uint8_t spgp_pgp_to_gcrypt_symmetric_algo(uint8_t pgpalgo);
                            
static uint8_t spgp_iv_length_for_symmetric_algo(uint8_t algo);

static uint8_t spgp_salt_length_for_hash_algo(uint8_t algo);




/**********************************************************************
**
** External function definitions
**
***********************************************************************/
#pragma mark External Function Definitions


spgp_packet_t *spgp_decode_message(uint8_t *message, uint32_t length) {
	spgp_packet_t *head = NULL;
  spgp_packet_t *pkt = NULL;
  uint32_t idx = 0;
  
	LOG_PRINT("begin\n");
  
	switch (setjmp(exception)) {
  	case 0:
    	break; /* Run logic */
    /* Below here are exceptions */
    default:
    	LOG_PRINT("Exception (0x%x)\n",_spgp_err);
      spgp_free_packet(&head);
  	  goto end;
  }

	if (NULL == message || 0 == length) {
  	RAISE(INVALID_ARGS);
  }
  
  // There must be at least one packet, yeah?
  head = malloc(sizeof(*head));
  if (NULL == head) RAISE(OUT_OF_MEMORY);
  memset(head, 0, sizeof(*head));
  pkt = head;
  
  
  // Loop to decode every packet in message
  while (idx < length-1) {
  
  	// Skip NULL bytes.  This is because 'partial packets' are a thing.  We
    // should theoretically always be on a valid packet boundary when we get
    // to this point.  However, if we are in the middle of processing a 
    // 'partial packet' we might be looking at a sub-header here instead of
    // the start of a new packet.  In the data packet decryption process, we
    // replace the sub-header with zeroes so we can detect it here and skip
    // over it.
		while (message[idx] == 0) idx++;

  	// Every packet starts with a header
    spgp_parse_header(message, &idx, length, pkt);
    if (!pkt->header) RAISE(FORMAT_UNSUPPORTED);
    
    // Decode packet contents based on the type marked in its header
    switch (pkt->header->type) {
    	case PKT_TYPE_USER_ID:
      	spgp_parse_user_id(message, &idx, length, pkt);
        break;
      case PKT_TYPE_PUBLIC_KEY:
      case PKT_TYPE_PUBLIC_SUBKEY:
      	spgp_parse_public_key(message, &idx, length, pkt);
        break;
      case PKT_TYPE_SECRET_KEY:
      case PKT_TYPE_SECRET_SUBKEY:
        spgp_parse_secret_key(message, &idx, length, pkt);
        break;
      case PKT_TYPE_SESSION:
      	spgp_parse_session_packet(message, &idx, length, pkt);
      	break;
      case PKT_TYPE_SYM_ENC_INT_DATA:
      	spgp_parse_encrypted_packet(message, &idx, length, pkt);
      	break;
      default:
        LOG_PRINT("WARNING: Unsupported packet type %u\n", pkt->header->type);
        // Increment to next packet.  We add the contentLength, but subtract
        // one parse_header() left us on the first byte of content.
        if (idx + pkt->header->contentLength - 1 < length)
          idx = idx + pkt->header->contentLength - 1;
        break;
    }
    
    // If we're at the end of the buffer, we're done
    if (idx >= length-1) break;
        
    // Allocate space for another packet
    pkt->next = malloc(sizeof(*pkt->next));
    if (NULL == pkt->next) RAISE(OUT_OF_MEMORY);
    memset(pkt->next, 0, sizeof(*pkt->next));
    pkt->next->prev = pkt; // make backwards pointer
    pkt = pkt->next;
    
    // Packet parser increments to it's own last byte.  Need one more to get
    // to the next packet's first byte. 
    SAFE_IDX_INCREMENT(idx, length);
	}
    
  end:
  LOG_PRINT("done\n");
  return head;
}

uint8_t spgp_decrypt_all_secret_keys(spgp_packet_t *msg, 
                                		 uint8_t *passphrase, uint32_t length) {
	spgp_packet_t *cur = msg;
  uint8_t err = 0;
  
	if (setjmp(exception)) {
    	LOG_PRINT("Exception (0x%x)\n",_spgp_err);
  	  goto end;
  }

	if (NULL == msg || NULL == passphrase || length == 0) RAISE(INVALID_ARGS);

	while ((cur = spgp_next_secret_key_packet(cur)) != NULL) {
  	LOG_PRINT("Decrypting secret key\n");
  	spgp_decrypt_secret_key(cur, passphrase, length);
  	cur = cur->next;
  }
  
  end:
  return err;
}


uint8_t spgp_load_keychain_with_keys(spgp_packet_t *msg) {
	spgp_packet_t *cur = msg;
  uint8_t err = 0;
  
	if (setjmp(exception)) {
    	LOG_PRINT("Exception (0x%x)\n",_spgp_err);
  	  goto end;
  }

	if (NULL == msg) RAISE(INVALID_ARGS);
	while ((cur = spgp_next_secret_key_packet(cur)) != NULL) {
  	LOG_PRINT("Adding key to keychain.\n");
  	cur = cur->next;
  }

	end:
  return err;
}


void spgp_free_packet(spgp_packet_t **pkt) {
	spgp_mpi_t *curMpi, *nextMpi;
  
	if (pkt == NULL)
  	return;
  if (*pkt == NULL)
  	return;
  
  //LOG_PRINT("Freeing packet: %p\n", pkt);
  
  // Recursively call on the next packet before freeing parent.
  if ((*pkt)->next) spgp_free_packet(&((*pkt)->next));
  
  // Release memory allocated for secret key fields
  if (((*pkt)->header->type == PKT_TYPE_SECRET_KEY ||
  		(*pkt)->header->type == PKT_TYPE_SECRET_SUBKEY) &&
      (*pkt)->c.secret != NULL) {
  	if ((*pkt)->c.secret->pub.mpiCount > 0) {
    	curMpi = (*pkt)->c.secret->pub.mpiHead;
      while (curMpi->next) {
      	nextMpi = curMpi->next;
        if (curMpi->data) free(curMpi->data);
        free(curMpi);
        curMpi = nextMpi;
      }
      (*pkt)->c.secret->pub.mpiHead = NULL;
      (*pkt)->c.secret->pub.mpiCount = 0;
    }
    if ((*pkt)->c.secret->s2kSalt) {
    	free((*pkt)->c.secret->s2kSalt);
      (*pkt)->c.secret->s2kSalt = NULL;
    }
    if ((*pkt)->c.secret->key) {
    	free((*pkt)->c.secret->key);
      (*pkt)->c.secret->key = NULL;
    }
    if ((*pkt)->c.secret->iv) {
    	free((*pkt)->c.secret->iv);
      (*pkt)->c.secret->iv = NULL;
    }
    free((*pkt)->c.secret);
    (*pkt)->c.secret = NULL;
  }
  else if ((*pkt)->header->type == PKT_TYPE_USER_ID &&
  				 (*pkt)->c.userid->data != NULL) {
  	free((*pkt)->c.userid->data);
    (*pkt)->c.userid->data = NULL;
    
    free((*pkt)->c.userid);
    (*pkt)->c.userid = NULL;
  }
  
  // release header
  if ((*pkt)->header) {
	  free((*pkt)->header);
	  (*pkt)->header = NULL;
  }
  
  // release packet
  free(*pkt);

  *pkt = NULL;
}

uint32_t spgp_err(void) {
	return _spgp_err;
}

const char *spgp_err_str(uint32_t err) {
	switch (err) {
  	case INVALID_ARGS:
    	return "Invalid arguments given to function.";
    case OUT_OF_MEMORY:
    	return "Not enough memory to continue parsing.";
    case INVALID_HEADER:
    	return "Invalid header format.  Corrupted or invalid data.";
    case FORMAT_UNSUPPORTED:
    	return "Message format is valid, but not currently supported.";
  	case BUFFER_OVERFLOW:
    	return "Index into buffer exceeded the maximum "
      	"bound of the buffer.";
    default:
    	return "Unknown/undocumented error.";
  }
}

uint8_t spgp_debug_log_enabled(void) {
	return debug_log_enabled;
}
void spgp_debug_log_set(uint8_t enable) {
	debug_log_enabled = enable;
}



/**********************************************************************
**
** Static function definitions
**
***********************************************************************/
#pragma mark Static Function Definitions


static uint8_t spgp_parse_header(uint8_t *msg, uint32_t *idx, 
														uint32_t length, spgp_packet_t *pkt) {
	uint8_t i;
  
	if (NULL == msg || NULL == pkt || NULL == idx || length == 0)
  	RAISE(INVALID_ARGS);

	// Allocate a header
	if (pkt->header == NULL) {
  	LOG_PRINT("Allocating header.\n");
  	pkt->header = malloc(sizeof(*(pkt->header)));
    if (pkt->header == NULL)
    	RAISE(OUT_OF_MEMORY);
    memset(pkt->header, 0, sizeof(*(pkt->header)));
  }
  
  // Header points to its parent packet
  pkt->header->parent = pkt;
  
  // The first byte is the 'tag byte', which tells us the packet type.
  pkt->header->rawTagByte = msg[*idx];
  SAFE_IDX_INCREMENT(*idx, length);
  LOG_PRINT("TAG BYTE: 0x%.2X\n", pkt->header->rawTagByte);
  
  // Validate tag byte -- top bit always set
  if (!(pkt->header->rawTagByte & 0x80))
  	RAISE(INVALID_HEADER);
    
  // Second-MSB tells us if this is new or old format header
  pkt->header->isNewFormat = pkt->header->rawTagByte & 0x40;
  
  // Read the packet type out of the tag byte
  if (pkt->header->isNewFormat)
  	pkt->header->type = pkt->header->rawTagByte & 0x1F;
  else // old style
  	pkt->header->type = (pkt->header->rawTagByte >> 2) & 0x0F;
  LOG_PRINT("TYPE: 0x%.2X\n", pkt->header->type);
  
  // Read the length of the packet's contents.  In old packets, the length
  // is encoded into the tag byte. 
  if (pkt->header->isNewFormat == 0) {
  	switch (pkt->header->rawTagByte & 0x03) {
    	case 0: pkt->header->headerLength = 2; break;
      case 1: pkt->header->headerLength = 3; break;
      case 2: pkt->header->headerLength = 5; break;
      default: 
      	// "indeterminate length" is supported by OpenPGP, but not us.
      	RAISE(FORMAT_UNSUPPORTED);
    }
    for (i = 0; i < pkt->header->headerLength - 1; i++) {
    	pkt->header->contentLength <<= 8;
      pkt->header->contentLength += msg[*idx];
      SAFE_IDX_INCREMENT(*idx, length);
    }
  }
  // In new packets, the length is encoded over a variable number of bytes, 
  // with the range of the first byte determining total number of bytes.
  else { // This is new style packet.
		pkt->header->contentLength = 
  		spgp_new_header_length(msg+*idx, 
      											 &(pkt->header->headerLength),
                             &(pkt->header->isPartial));
    *idx += pkt->header->headerLength - 2;
    SAFE_IDX_INCREMENT(*idx, length);
  }
  
  LOG_PRINT("LENGTH: %u\n", pkt->header->contentLength);
  
	return 0;
}

static uint32_t spgp_new_header_length(uint8_t *header, 
																			uint8_t *header_len,
                                      uint8_t *is_partial) {
  uint32_t content;
  uint8_t len[4];
  uint8_t i = 0;
  
  if (NULL == header || NULL == header_len) RAISE(INVALID_ARGS);
  
  *is_partial = 0; // default to known length
  
  len[0] = header[i];
  if (len[0] <= 191) { // 1-byte length
    *header_len = 2;
    content = len[0];
  }
  else if (len[0] > 191 && len[0] <= 223) { // 2-byte length
    *header_len = 3;
    len[1] = header[i+1]; 
    content = ((len[0]-192)<<8) | (len[1] + 192);
  }
  else if (len[0] == 255) { // 5-byte length
    *header_len = 5;
    len[0] = header[i+1];
    len[1] = header[i+2];
    len[2] = header[i+3];
    len[3] = header[i+4];
    content = (len[0]<<24) | (len[1]<<16) | (len[2]<<8) | len[3];
  }
  else {
    // indeterminate length
    LOG_PRINT("Partial length header!\n");
    *header_len = 2;
    *is_partial = 1;
    content = 1 << (len[0] & 0x1F);
  }
	return content;
}

static uint8_t spgp_parse_user_id(uint8_t *msg, uint32_t *idx, 
          												uint32_t length, spgp_packet_t *pkt) {
	spgp_userid_pkt_t *userid;

  LOG_PRINT("Parsing user id.\n");

	// Make sure we have enough bytes remaining for the copy
  if (length - *idx < pkt->header->contentLength) RAISE(BUFFER_OVERFLOW);
  
  // Allocate userid field in packet
  pkt->c.userid = malloc(sizeof(*(pkt->c.userid)));
  if (NULL == pkt->c.userid) RAISE(OUT_OF_MEMORY);
  userid = pkt->c.userid;
  
  // Allocate space for buffer, plus one byte for NUL terminator
	userid->data = malloc(sizeof(*(userid->data))*pkt->header->contentLength + 1);
  if (NULL == userid->data) RAISE(OUT_OF_MEMORY);
  
  // Copy bytes from input to structure, and add a NUL terminator
  memcpy(userid->data, msg+*idx, pkt->header->contentLength);
  userid->data[pkt->header->contentLength] = '\0';
  *idx += pkt->header->contentLength - 1;

  setlocale(LC_CTYPE, "en_US.UTF-8");
  wprintf(L"USER ID: %s\n", pkt->c.userid->data);
  
	return 0;                                     
}

static uint8_t spgp_generate_fingerprint(spgp_packet_t *pkt) {
	uint16_t packetSize;
  uint8_t packetHeaderSize;
  spgp_mpi_t *curMpi;
  uint8_t targetMpiCount;
  gcry_md_hd_t md;
  unsigned char *hash;
  int i;
  
  if (NULL == pkt) RAISE(INVALID_ARGS);
  
  // Start with header info: 1 version, 4 creation time, 1 algorithm
  packetHeaderSize = sizeof(pkt->c.pub->version) +
  	sizeof(pkt->c.pub->creationTime) +
    sizeof(pkt->c.pub->asymAlgo);
  packetSize = packetHeaderSize;

  // Figure out how many MPIs to add
  switch(pkt->c.pub->asymAlgo) {
  	case ASYM_ALGO_DSA:
    	targetMpiCount = 4;
      break;
    case ASYM_ALGO_ELGAMAL:
    	targetMpiCount = 3;
      break;
    default:
    	RAISE(FORMAT_UNSUPPORTED);
  }
  
  // Add size of each MPI
  curMpi = pkt->c.pub->mpiHead;
  i = 0;
  while (curMpi && i < targetMpiCount) {
  	packetSize += curMpi->count + 2; // add 2 for MPI header
		curMpi = curMpi->next;
    i++;
  }
      
  // Give data to hash to gcrypt
	if (gcry_md_open (&md, GCRY_MD_SHA1, 0) != 0) RAISE(GCRY_ERROR);
  gcry_md_putc(md, 0x99 );
  gcry_md_putc(md, packetSize >> 8);
  gcry_md_putc(md, packetSize);
  gcry_md_putc(md, pkt->c.pub->version);
  gcry_md_putc(md, pkt->c.pub->creationTime);
  gcry_md_putc(md, pkt->c.pub->creationTime >> 8);
  gcry_md_putc(md, pkt->c.pub->creationTime >> 16);
  gcry_md_putc(md, pkt->c.pub->creationTime >> 24);
  gcry_md_putc(md, pkt->c.pub->asymAlgo);
  
	// Write the public key MPIs
  curMpi = pkt->c.pub->mpiHead;
  i = 0;
  while (curMpi && i < targetMpiCount) {
  	gcry_md_write(md, curMpi->data, curMpi->count + 2);
		curMpi = curMpi->next;
    i++;
  }
  
  // Perform SHA-1 hash
  gcry_md_final(md);
  hash = gcry_md_read(md, 0);

	// Copy hash results (20-bytes) into fingerprint
  pkt->c.pub->fingerprint = malloc(20);
  if (NULL == pkt->c.pub->fingerprint) RAISE(OUT_OF_MEMORY);
  memcpy(pkt->c.pub->fingerprint, hash, 20);
  
  LOG_PRINT("HASH: ");
  for (targetMpiCount=0; targetMpiCount < 20; targetMpiCount++) {
  	fprintf(stderr, "%.2X", pkt->c.pub->fingerprint[targetMpiCount]);
  }
  fprintf(stderr,"\n");
  
  gcry_md_close(md);
  
  return 0;
}

static uint8_t spgp_verify_decrypted_data(uint8_t *data, uint32_t length) {
  gcry_md_hd_t md;
  uint32_t hashlen = length - 20; // SHA1 hash is 20 bytes
  uint8_t *hashResult;
  int result;
  
  if (gcry_md_open (&md, GCRY_MD_SHA1, 0) != 0) RAISE(GCRY_ERROR);
  gcry_md_write(md, data, hashlen);
  gcry_md_final(md);
  hashResult = gcry_md_read(md, 0);
  if (NULL == hashResult) RAISE(GCRY_ERROR);
	result = memcmp(data+hashlen, hashResult, 20);
	gcry_md_close(md);
	return result;
}

/**
 * Generate cipher key to decrypt secret key packet
 *
 * The secret portion of a secret key packet can be encrypted with a 
 * symmetric cipher.  This function generates the 'key' that is used as
 * the input to the symmetric cipher.  This key is generated by hashing
 * a randomly generated salt, included in the packet, and the user's
 * passphrase (which must be provided).
 *
 * OpenPGP's standard allows multiple ways of generating the key by varying
 * the hash algorithm.
 *
 * @param pkt A secret key or secret subkey packet
 * @param passphrase User's passphrase to decrypt with
 * @param length Length (in bytes) of user's passphrase
 *
 * @return 0 on success.  Raises exception on error.
 *
 */
static uint8_t spgp_generate_cipher_key(spgp_packet_t *pkt,
																			  uint8_t *passphrase, uint32_t length) {
	spgp_secret_pkt_t *secret;
  spgp_public_pkt_t *pub;
  gcry_md_hd_t md;
  uint32_t i;
  uint32_t keyBytesRemaining;// Bytes left to generate for key
  uint32_t hashLen;          // How long the hash is (algo-dependent)
  uint32_t hashIters;        // How many hash results to combine into key
  uint32_t hashBytes;        // Total number of bytes to hash each time
  uint32_t bufLen;           // Length of salt+passphrase
  uint32_t curHashCount;     // How many times we have performed full hash
  uint32_t hashCopies;       // How many integer copies of hashBuf per round
  uint32_t hashExtraBytes;   // How many extra bytes to hash for last round
  uint8_t *hashBuf;          // Store concatenated salt+passphrase
  uint8_t *hashResult;       // Store result of actual hash algorithm
  
  if (NULL == pkt || NULL == passphrase) RAISE(INVALID_ARGS);
  
	secret = pkt->c.secret;
  pub = pkt->c.pub;

	if (pkt->header->type != PKT_TYPE_SECRET_KEY &&
  		pkt->header->type != PKT_TYPE_SECRET_SUBKEY)
      RAISE(INVALID_ARGS);
  
  // Determine how many bytes we need to produce for this cipher
  // Only supporting 3DES for this
  switch(secret->s2kEncryption) {
  case SYM_ALGO_3DES: secret->keyLength = 24; break;
  case SYM_ALGO_CAST5: secret->keyLength = 16; break;
  default: RAISE(FORMAT_UNSUPPORTED);
  }
   
  
  // Initialize hash algorithm, determine how many bytes produces per round
	switch (secret->s2kHashAlgo) {  
  	case HASH_ALGO_SHA1:
  		if (gcry_md_open (&md, GCRY_MD_SHA1, 0) != 0) RAISE(GCRY_ERROR);
      hashLen = 20;
      break;
		default:
    	RAISE(FORMAT_UNSUPPORTED);
      break;
  }
  
  // Determine how many times we have to hash to generate a large enough key
  // Ex: 3DES needs 24 bytes, SHA1 makes 20 bytes, so need to SHA1 hashes.
  hashIters = (secret->keyLength/hashLen) + (secret->keyLength%hashLen>0)?1:0;
  
  // What hashing mode to use.
  // Currently only supporting salted+iterated
  switch (secret->s2kSpecifier) {
  	case S2K_TYPE_ITERATED:
    	break;
    default:
    	RAISE(FORMAT_UNSUPPORTED);
      break;
  }
   
  // Allocate space for the key
  secret->key = malloc(secret->keyLength);
  if (NULL == secret->key) RAISE(OUT_OF_MEMORY);
  
  // Allocate a buffer to store the salt and passphrase combined
  // Since this buffer is local only, no exceptions can be raised after
  // this point or memory will be leaked.
  bufLen = secret->s2kSaltLength + length;
  hashBuf = malloc(bufLen);
  if (NULL == hashBuf) RAISE(OUT_OF_MEMORY);
  
  // Concatenate salt and passphrase into hashBuf
  memcpy(hashBuf, secret->s2kSalt, secret->s2kSaltLength);
  memcpy(hashBuf + secret->s2kSaltLength, passphrase, length);
  
  // Magic formula from RFC 4880.  This is number of bytes to hash over.
  hashBytes = (16 + (secret->s2kCount & 15)) << ((secret->s2kCount >> 4) + 6);
  
  // Figure out how many times to iterate over hashBuf to get hashBytes,
  // and how many extra bytes are needed at the end if not an even multiple.
  hashCopies = hashBytes / (bufLen);
  hashExtraBytes = hashBytes % (bufLen);
  
  keyBytesRemaining = secret->keyLength;
  
  // Loop until we have enough hash bytes to make the key
  curHashCount = 0;
  while (curHashCount <= hashIters && keyBytesRemaining) {
    for (i = 0; i < curHashCount; i++) {
    	// pad front with 1 NUL byte per round (none on first round)
      gcry_md_putc(md, '\0'); 
    }
    // Copy the salt+passphrase combo into hash buffer as many times as fits
    for (i = 0; i < hashCopies; i++) {
    	gcry_md_write(md, hashBuf, bufLen);
    }
    // Copy any leftover bytes into hash buffer to reach |hashBytes|
    if (hashExtraBytes) {
    	gcry_md_write(md, hashBuf, hashExtraBytes);
    }
    // Perform the hash and append to the key
	  gcry_md_final(md);
  	hashResult = gcry_md_read(md, 0);
    
    if (keyBytesRemaining < hashLen) {
      memcpy(secret->key+(curHashCount*hashLen), 
             hashResult, 
             keyBytesRemaining);   
      keyBytesRemaining = 0;
    }
    else {
      memcpy(secret->key+(curHashCount*hashLen), 
             hashResult, 
             hashLen);
      keyBytesRemaining -= hashLen;
    }
    // Reset hash algorithm for next round
    gcry_md_reset(md);
    curHashCount++;
  }

	gcry_md_close(md);
	free(hashBuf);
	return 0;
}

static uint8_t spgp_parse_public_key(uint8_t *msg, uint32_t *idx, 
          													 uint32_t length, spgp_packet_t *pkt) {
  spgp_public_pkt_t *pub;
  
  LOG_PRINT("Parsing public key.\n");

	// Make sure we have enough bytes remaining for parsing
  if (length - *idx < pkt->header->contentLength) RAISE(BUFFER_OVERFLOW);

	// Allocate public key if it doesn't already exist.  It might exist if
  // this packet is a secret key.
  if (!(pkt->c.pub)) {
    pkt->c.pub = malloc(sizeof(*(pkt->c.pub)));
    if (NULL == pkt->c.pub) RAISE(OUT_OF_MEMORY);
    memset(pkt->c.pub, 0, sizeof(*(pkt->c.pub)));
	}
  
  pub = pkt->c.pub;
  
  pub->version = msg[*idx]; 
  SAFE_IDX_INCREMENT(*idx, length);
  
  // First byte is the version.
  if (pub->version != 4) RAISE(FORMAT_UNSUPPORTED);
  
  // Next 4 bytes are big-endian 'key creation time'
  if (length - *idx < 4) RAISE(BUFFER_OVERFLOW);
  memcpy(&(pub->creationTime), msg+*idx, 4);
  *idx += 3; // this puts us on last byte of creation time
  SAFE_IDX_INCREMENT(*idx, length); // this goes to next byte (safely)

	// Next byte identifies asymmetric algorithm
	pub->asymAlgo = msg[*idx];
	SAFE_IDX_INCREMENT(*idx, length);
  LOG_PRINT("Asymmetric algorithm: %d\n", pub->asymAlgo);
  
  // Read variable number of MPIs (depends on asymmetric algorithm), each
  // of which are variable size.
	spgp_read_all_public_mpis(msg, idx, length, pkt->c.pub);
  LOG_PRINT("Read %u MPIs\n", pub->mpiCount);
  
  return 0;
}

static uint8_t spgp_parse_secret_key(uint8_t *msg, uint32_t *idx, 
          													 uint32_t length, spgp_packet_t *pkt) {
  spgp_secret_pkt_t *secret;
  spgp_public_pkt_t *pub;
  uint32_t startIdx = *idx;
  
  LOG_PRINT("Parsing secret key.\n");

	// Make sure we have enough bytes remaining for parsing
  if (length - *idx < pkt->header->contentLength) RAISE(BUFFER_OVERFLOW);

	// Allocate secret key in packet  
  pkt->c.secret = malloc(sizeof(*(pkt->c.secret)));
  if (NULL == pkt->c.secret) RAISE(OUT_OF_MEMORY);
  memset(pkt->c.secret, 0, sizeof(*(pkt->c.secret)));

	secret = pkt->c.secret;
  pub = pkt->c.pub;

	// Parse the public key section that starts it
	spgp_parse_public_key(msg, idx, length, pkt);
  // idx ends on last byte of public key.  One more to start secret key.
  SAFE_IDX_INCREMENT(*idx, length);
  
  // S2K Type byte tells how to (or if to) decrypt secret exponent
  secret->s2kType = msg[*idx];
  SAFE_IDX_INCREMENT(*idx, length);
  switch (secret->s2kType) {
  	case 0:
    	// There is no encryption
    	secret->s2kEncryption = 0;
      break;
    case 254:
    case 255:
    	// Next byte is encryption type
		  secret->s2kEncryption = msg[*idx];
  		SAFE_IDX_INCREMENT(*idx, length);
			break;
    default:
    	// This byte is encryption type
    	secret->s2kEncryption = secret->s2kType;
    	break;
  }
  LOG_PRINT("Encryption: %u\n", secret->s2kEncryption);
  
  if (secret->s2kEncryption) {
  	// Secret exponent is encrypted (as it should be).  Time to decrypt.
    
    // S2K specifier tells us if there is a salt, and how to use it
    if (secret->s2kType >= 254) {
			secret->s2kSpecifier = msg[*idx];
  	  SAFE_IDX_INCREMENT(*idx, length);
   	 LOG_PRINT("S2K Specifier: %u\n", secret->s2kSpecifier);
    }
    
    // S2K hash algorithm specifies how to hash passphrase into a key
    secret->s2kHashAlgo = msg[*idx];
    SAFE_IDX_INCREMENT(*idx, length);
    LOG_PRINT("Hash algorithm: %u\n", secret->s2kHashAlgo);    
    
    // Read the salt if there is one
    switch (secret->s2kSpecifier) {
    	case 1:
      	spgp_read_salt(msg, idx, length, secret);
      	break;
      case 3:
      	spgp_read_salt(msg, idx, length, secret);
        // S2K Count is number of bytes to hash to make the key
				secret->s2kCount = msg[*idx];
    		SAFE_IDX_INCREMENT(*idx, length);
        break;
      default:
      	break;
    }
  }
  LOG_PRINT("Salt length: %u\n", secret->s2kSaltLength);
  
  // If it's not encrypted, we can just read the secret MPIs
  if (!secret->s2kEncryption) {
  	spgp_read_all_secret_mpis(msg, idx, length, secret);
  }
  // If it is encrypted, just store it for now.  We'll decrypt later.
  else {
  
  	// There's an initial vector (IV) here:
  	spgp_read_iv(msg, idx, length, secret);
    LOG_PRINT("IV length: %u\n", secret->ivLength);
  
  	// Figure out how much is left, and make sure it's available
  	uint32_t packetOffset = *idx - startIdx;
  	uint32_t remaining = pkt->header->contentLength - packetOffset;
		if (packetOffset >= pkt->header->contentLength) RAISE(BUFFER_OVERFLOW);
    
    // Allocate buffer and copy data
  	secret->encryptedData = malloc(remaining);
    if (NULL == secret->encryptedData) RAISE(OUT_OF_MEMORY);
    memcpy(secret->encryptedData, msg+*idx, remaining);
    secret->encryptedDataLength = remaining;
    
    *idx += remaining-1;
    LOG_PRINT("Stored %u encrypted bytes.\n", remaining);
    // This is the end of the data, so we do NOT do a final idx increment
  }
  
  // Create and store fingerprint for this packet
  spgp_generate_fingerprint(pkt);
    
	return 0;
}

static spgp_packet_t *spgp_next_secret_key_packet(spgp_packet_t *msg) {
	spgp_packet_t *cur = msg;
	while (cur) {
  	if (cur->header->type == PKT_TYPE_SECRET_KEY ||
    		cur->header->type == PKT_TYPE_SECRET_SUBKEY)
        return cur;
  	cur = cur->next;
  }
  return NULL;
}

static uint8_t spgp_decrypt_secret_key(spgp_packet_t *pkt, 
                                			 uint8_t *passphrase, uint32_t length) {
  gcry_cipher_hd_t hd;
	spgp_secret_pkt_t *secret;
  spgp_public_pkt_t *pub;
  spgp_mpi_t *curMpi;
  uint32_t idx;
  uint32_t secretMpiCount;
  uint8_t *secdata;
  uint8_t i;
  uint8_t err = 0;

	if (NULL == pkt || NULL == passphrase || length == 0) RAISE(INVALID_ARGS);

	secret = pkt->c.secret;
  pub = pkt->c.pub;
  
	if (pkt->header->type != PKT_TYPE_SECRET_KEY &&
  		pkt->header->type != PKT_TYPE_SECRET_SUBKEY)
      RAISE(INVALID_ARGS);

  if (secret->isDecrypted) return err; // already decrypted!
      
  spgp_generate_cipher_key(pkt, passphrase, length);

  switch (secret->s2kEncryption) {
  	case SYM_ALGO_3DES:
    case SYM_ALGO_CAST5:
		  if (gcry_cipher_open(&hd,
      										 spgp_pgp_to_gcrypt_symmetric_algo(secret->s2kEncryption), 
                           GCRY_CIPHER_MODE_CFB, 
    	    	               GCRY_CIPHER_SECURE | GCRY_CIPHER_ENABLE_SYNC) != 0)
      	RAISE(GCRY_ERROR);
      break;
    default:
    	RAISE(FORMAT_UNSUPPORTED);
	}

	if (NULL == secret->key || NULL == secret->iv) RAISE(INCOMPLETE_PACKET);

	if (gcry_cipher_setkey(hd, secret->key, secret->keyLength) != 0)
  	RAISE(GCRY_ERROR);
	if (gcry_cipher_setiv(hd, secret->iv, secret->ivLength) != 0)
  	RAISE(GCRY_ERROR);
    
  // Allocate secret data memory.  Must free it before raising any exceptions!
  secdata = malloc(secret->encryptedDataLength);
  if (NULL == secdata) RAISE(OUT_OF_MEMORY);
  if (gcry_cipher_decrypt(hd, 
  												secdata, 
  												secret->encryptedDataLength, 
      		                secret->encryptedData, 
                          secret->encryptedDataLength) != 0) {
    free(secdata);
  	RAISE(GCRY_ERROR);
  }
  
  // Verify checksum
  if (spgp_verify_decrypted_data(secdata, secret->encryptedDataLength) != 0)
  	RAISE(DECRYPT_FAILED);
  
  // Decode and store the secret MPIs (algo-specific):
  switch(pub->asymAlgo) {
  	case ASYM_ALGO_DSA:
    case ASYM_ALGO_ELGAMAL:
    	secretMpiCount = 1;
      break;
    default:
    	RAISE(FORMAT_UNSUPPORTED);
  }
    
  // Get to the last valid MPI
  if (NULL == pub->mpiHead) RAISE(INCOMPLETE_PACKET);
  curMpi = pub->mpiHead;
  while (curMpi->next) curMpi = curMpi->next;
  
  idx = 0;
  for (i = 0; i < secretMpiCount; i++) {
	  curMpi->next = spgp_read_mpi(secdata, &idx, secret->encryptedDataLength);
    if (NULL == curMpi->next) RAISE(GENERIC_ERROR);
    curMpi = curMpi->next;
    pub->mpiCount++;
  }
  secret->isDecrypted = 1;
  
  gcry_cipher_close(hd);
  free(secdata);
  
  end:
	return err;
}

static uint8_t spgp_parse_encrypted_packet(uint8_t *msg, 
                                           uint32_t *idx, 
          														 		 uint32_t length, 
                                           spgp_packet_t *pkt) {
  spgp_packet_t *session_pkt;
  spgp_session_pkt_t *session;
  gcry_cipher_hd_t cipher_hd;
	gcry_error_t err;
  int version;
  unsigned long blksize;
  uint32_t encbytes;
  uint32_t startidx;
  uint32_t i;
  uint8_t headerlen;
  uint8_t is_done;
  uint8_t is_partial;
  
  if (NULL == msg || NULL == idx || length == 0 || NULL == pkt)
  	RAISE(INVALID_ARGS);
    
  version = msg[*idx];
  SAFE_IDX_INCREMENT(*idx, length);
  
  // As of this writing, only version 1 exists
  if (version != 1) RAISE(FORMAT_UNSUPPORTED);
  
  session_pkt = spgp_find_session_packet(pkt);
  if (NULL == session_pkt) {
  	LOG_PRINT("No session key found!\n");
  	RAISE(DECRYPT_FAILED);
  }
  session = session_pkt->c.session;
  
  startidx = *idx;
  is_done = 0;
  is_partial = pkt->header->isPartial;
  
  // Drop 1 from contentLength to account for version
  encbytes = pkt->header->contentLength - 1;
  
  // Since data packets can have partial length, we loop and decrypt
  // here until the whole blasted thing is decrypted.
  while (!is_done) {
    err = gcry_cipher_open (&cipher_hd, 
            spgp_pgp_to_gcrypt_symmetric_algo(session->symAlgo),
            GCRY_CIPHER_MODE_CFB,
            (GCRY_CIPHER_SECURE | GCRY_CIPHER_ENABLE_SYNC |
            GCRY_CIPHER_ENABLE_SYNC));
           
    blksize = spgp_iv_length_for_symmetric_algo(session->symAlgo);
           
    err |= gcry_cipher_setkey(cipher_hd, session->key, session->keylen);
    err |= gcry_cipher_setiv(cipher_hd, 0, blksize);
    err |= gcry_cipher_decrypt(cipher_hd, 
          	                   msg+*idx, 
                               encbytes, 
                               NULL, 
                               0);
    if (err) RAISE(GCRY_ERROR);
    
    *idx += encbytes - 1; // increment to last byte of data
    
    // Check if we're done
    if (!(is_partial)) {
    	is_done = 1;
      continue;
    }
    
    // We are processing a partial packet, so figure out next length
	  SAFE_IDX_INCREMENT(*idx, length); // inc to first byte of header
    encbytes = spgp_new_header_length(msg+*idx, 
               						            &(headerlen),
                          						&(is_partial));
    // Change the header bytes to zero so the packet parser knows to skip them
    // later.
    for (i = 0; i < headerlen; i++) {
    	msg[*idx + i] = 0x00;
    }
    // Move ahead to the next data byte
    *idx += headerlen - 2;
	  SAFE_IDX_INCREMENT(*idx, length); // inc to first byte of data
  }

	// Validate decryption with PGP's MDC doo-hickey.  
  if (memcmp(msg+startidx+blksize-2, msg+startidx+blksize, 2) != 0) {
  	LOG_PRINT("Decrypted data block fails validation!\n");
    RAISE(DECRYPT_FAILED);
  }

  // At this point, msg has been decrypted in place and now contains
  // a bunch of packets.  Since it was decoded in place, and since we're
  // already in the middle of a packet decode loop, we can just teleport
  // idx back to the beginning of the data and exit.
  //
  // Packet data starts blocksize+2 bytes above decrypted data.  One block
  // of random data, and 2 extra bytes of verification.
  //
  // Subtract 1 because startidx is the first byte of the decrypted packet, 
  // but the packet parser loop expects us to end on the last byte of the 
  // previous packet.
  *idx = startidx + blksize + 2 - 1;
  
	return 0;
}

static spgp_packet_t *spgp_find_session_packet(spgp_packet_t *chain) {
	spgp_packet_t *cur;
  
  if (NULL == chain) RAISE(INVALID_ARGS);
  
  cur = chain;
  
  while (cur) {
    if (cur->header->type == PKT_TYPE_SESSION &&
        cur->c.session->key != NULL)
        return cur;
    cur = cur->prev;
	}
    
  return NULL;
}

static uint8_t spgp_parse_session_packet(uint8_t *msg, uint32_t *idx, 
          													 		 uint32_t length, spgp_packet_t *pkt) {
	spgp_session_pkt_t *session;
  spgp_packet_t *key, *chain;
  gcry_sexp_t sexp_key, sexp_data, sexp_result;
  gcry_mpi_t mpis[10], mpi_result;
  spgp_mpi_t *cur;
  uint32_t checksum, sum;
  int i;
  unsigned long frame_len;
  uint8_t *frame;
  
  LOG_PRINT("Parsing session packet.\n");

	if (NULL == msg || NULL == idx || length == 0 || NULL == pkt)
  	RAISE(INVALID_ARGS);

	// Make sure we have enough bytes remaining for parsing
  if (length - *idx < pkt->header->contentLength) RAISE(BUFFER_OVERFLOW);

	// Allocate a session packet
	pkt->c.session = malloc(sizeof(*(pkt->c.session)));
  if (NULL == pkt->c.session) RAISE(OUT_OF_MEMORY);
  memset(pkt->c.session, 0, sizeof(*(pkt->c.session)));

	session = pkt->c.session;	
  
  session->version = msg[*idx];
  SAFE_IDX_INCREMENT(*idx, length);
  LOG_PRINT("Version: %u\n", session->version);

	memcpy(session->keyid, msg+*idx, 8);
  *idx += 7;
  SAFE_IDX_INCREMENT(*idx, length);
  LOG_PRINT("Session for key ID: ");
  for (i = 0; i < 8; i++) printf("%.2X",session->keyid[i]);
  printf("\n");

  session->algo = msg[*idx];
  SAFE_IDX_INCREMENT(*idx, length);
	
  // Read first MPI.  RSA only has one
  session->mpi1 = spgp_read_mpi(msg, idx, length);
  // Elgamal has a second MPI
	if (session->algo == ASYM_ALGO_ELGAMAL) {
	  SAFE_IDX_INCREMENT(*idx, length);    
  	session->mpi2 = spgp_read_mpi(msg, idx, length);
  }
  
  // DONE READING FROM STREAM AT THIS POINT
  // BELOW HERE -- DECRYPT SESSION KEY
  
  if (!spgp_keychain_is_valid()) RAISE(KEYCHAIN_ERROR);
  spgp_keychain_iter_start();
  while ((chain = spgp_keychain_iter_next()) != NULL) {
  	if ((key = spgp_secret_key_matching_id(chain, session->keyid)) != NULL) {
    	LOG_PRINT("Found a matching key in keychain.\n");
      break;
    }
  }
  spgp_keychain_iter_end();
  
  if (!key) return -1;
  
  
  for (cur=key->c.pub->mpiHead,i = 0; cur != NULL; cur = cur->next,i++) {
	  gcry_mpi_scan (&(mpis[i]), GCRYMPI_FMT_PGP, cur->data, cur->count+2, NULL);
  }
  gcry_mpi_scan (&(mpis[i++]), GCRYMPI_FMT_PGP, 
                 session->mpi1->data, session->mpi1->count+2, NULL);
  if (session->mpi2) {
  	gcry_mpi_scan (&(mpis[i++]), GCRYMPI_FMT_PGP, 
                   session->mpi2->data, session->mpi2->count+2, NULL);
  }

  
  switch (session->algo) {
  	case ASYM_ALGO_ELGAMAL:
		  gcry_sexp_build(&sexp_key, NULL,
				"(private-key(elg(p%m)(g%m)(y%m)(x%m)))",
				mpis[0], mpis[1], mpis[2], mpis[3]);
		  gcry_sexp_build (&sexp_data, NULL,
			   "(enc-val(elg(a%m)(b%m)))", mpis[4], mpis[5]);
		  gcry_pk_decrypt (&sexp_result, sexp_data, sexp_key);
  		mpi_result = gcry_sexp_nth_mpi (sexp_result, 0, GCRYMPI_FMT_STD);
    	break;
    default:
    	RAISE(FORMAT_UNSUPPORTED);
  }

  gcry_mpi_print(GCRYMPI_FMT_PGP, NULL, 0, &frame_len, mpi_result);
  frame = malloc(frame_len);
  if (NULL == frame) RAISE(OUT_OF_MEMORY);
  gcry_mpi_print(GCRYMPI_FMT_PGP, frame, frame_len, NULL, mpi_result);

	i = 2; // skip first two bytes, they're the length of the mpi
  if (frame[i++] != 2) RAISE(DECRYPT_FAILED);

	while (frame[i++] != 0 && i < frame_len) ; // Find the next 0 in frame
  
  // Algorithm is first byte after the 0
  session->symAlgo = frame[i];
  
  // Key length is determined from current index.  Drop 3 bytes: 1 for
  // algorithm, and 2 for the checksum at the end.
  session->keylen = frame_len - i - 3;
	i++;

	// Actual session key is the remaining bytes, except for the last two
	session->key = malloc(session->keylen);
  if (NULL == session->key) RAISE(OUT_OF_MEMORY);
  if (i+session->keylen >= frame_len) RAISE(DECRYPT_FAILED);
	memcpy(session->key, frame+i, session->keylen);

	// Checksum is last two bytes in buffer
	checksum = frame[frame_len-2]<<8 | frame[frame_len-1];
  
  // Verify checksum
  sum = 0;
  for (i = 0; i < session->keylen; i++) {
  	sum = sum + (session->key[i] & 0xFF);
  }
  if (sum % 65536 != checksum) {
  	LOG_PRINT("Session key checksum failed!\n");
  	RAISE(DECRYPT_FAILED);
  }
	LOG_PRINT("Decrypted session key.\n");
  return 0;
}

/**
 * Find a specific secret key in a chain of packets.
 *
 * Finds a secret key in the given chain of packets, |chain|, with a key ID
 * matching |keyid|.  KeyID is the last 8 octets of the 64-octet key
 * fingerprint.
 *
 * @param chain Chain of packets containing at least one secret key
 * @param keyid 8-octet key ID
 * @return Packet containing matching secret key, or NULL if not found
 *
 */
static spgp_packet_t *spgp_secret_key_matching_id(spgp_packet_t *chain,
																									uint8_t *keyid) {
	spgp_packet_t *cur = NULL;
  
  if (NULL == chain || NULL == keyid) RAISE(INVALID_ARGS);
  
  cur = chain;
	while ((cur = spgp_next_secret_key_packet(cur)) != NULL) {
		if (memcmp((void*)((cur->c.pub->fingerprint)+12),keyid,8) == 0)
    	return cur;
  	cur = cur->next;
  }
  
  return NULL;
}

static uint8_t spgp_read_salt(uint8_t *msg, 
                              uint32_t *idx,
                              uint32_t length, 
                              spgp_secret_pkt_t *secret) {
	uint8_t saltLen = 0;
  
 	if (NULL == msg || NULL == idx || 0 == length || NULL == secret)
  	RAISE(INVALID_ARGS);
  
  if ((saltLen = spgp_salt_length_for_hash_algo(secret->s2kHashAlgo)) == 0) 
  	RAISE(FORMAT_UNSUPPORTED);
  
  if (length - *idx < saltLen) RAISE(BUFFER_OVERFLOW);
  
  secret->s2kSalt = malloc(sizeof(*(secret->s2kSalt)) * saltLen);
  if (NULL == secret->s2kSalt) RAISE(OUT_OF_MEMORY);
  
  secret->s2kSaltLength = saltLen;
  memcpy(secret->s2kSalt, msg+*idx, saltLen);
  *idx += saltLen-1;
  SAFE_IDX_INCREMENT(*idx, length);
  
	return 0;
}

static uint8_t spgp_read_iv(uint8_t *msg, 
                            uint32_t *idx,
                            uint32_t length, 
                            spgp_secret_pkt_t *secret) {
	uint8_t ivLen = 0;
  
 	if (NULL == msg || NULL == idx || 0 == length || NULL == secret)
  	RAISE(INVALID_ARGS);
  
  if ((ivLen = spgp_iv_length_for_symmetric_algo(secret->s2kEncryption)) == 0) 
  	RAISE(FORMAT_UNSUPPORTED);
  
  if (length - *idx < ivLen) RAISE(BUFFER_OVERFLOW);
  
  secret->iv = malloc(sizeof(*(secret->iv)) * ivLen);
  if (NULL == secret->iv) RAISE(OUT_OF_MEMORY);
  
  secret->ivLength = ivLen;
  memcpy(secret->iv, msg+*idx, ivLen);
  *idx += ivLen-1;
  SAFE_IDX_INCREMENT(*idx, length);
  
	return 0;
}

static uint8_t spgp_pgp_to_gcrypt_symmetric_algo(uint8_t pgpalgo) {
  switch (pgpalgo) {
  case 1: return GCRY_CIPHER_IDEA;
  case 2: return GCRY_CIPHER_3DES;
  case 3: return GCRY_CIPHER_CAST5;
  case 4: return GCRY_CIPHER_BLOWFISH;
  case 7: return GCRY_CIPHER_AES128;
  case 8: return GCRY_CIPHER_AES192;
  case 9: return GCRY_CIPHER_AES256;
  case 10:return GCRY_CIPHER_TWOFISH;
  default: return 0xFF;
  }
}

static uint8_t spgp_iv_length_for_symmetric_algo(uint8_t algo) {
	size_t ivlen = 0;
	if (gcry_cipher_algo_info(spgp_pgp_to_gcrypt_symmetric_algo(algo), 
  													GCRYCTL_GET_BLKLEN, 
                            NULL, 
                            &ivlen) != 0)
  	RAISE(FORMAT_UNSUPPORTED);
  return ivlen;
}

static uint8_t spgp_salt_length_for_hash_algo(uint8_t algo) {
	if (algo == HASH_ALGO_SHA1) return 8;
  else RAISE(FORMAT_UNSUPPORTED); // not implemented
  return 0;
}

static uint8_t spgp_read_all_public_mpis(uint8_t *msg, 
                                         uint32_t *idx,
														 						 uint32_t length, 
                                         spgp_public_pkt_t *pub) {
  spgp_mpi_t *curMpi, *newMpi;
  uint32_t i;
  uint8_t mpiCount;
  
  if (NULL == msg || NULL == idx || 0 == length || NULL == pub)
  	RAISE(INVALID_ARGS);

	switch (pub->asymAlgo) {
  	case ASYM_ALGO_DSA: mpiCount = 4; break;
    case ASYM_ALGO_ELGAMAL: mpiCount = 3; break;
    default: RAISE(FORMAT_UNSUPPORTED);
  }

  // Read all the MPIs
  for (i = 0; i < mpiCount; i++) {
  	// spgp_read_mpi() doesn't increment past the end of the MPI, so if this
    // isn't the first pass we need to increment once more
  	if (i) SAFE_IDX_INCREMENT(*idx, length);    
    newMpi = spgp_read_mpi(msg, idx, length);
    if (i == 0) {
      pub->mpiHead = newMpi;
      curMpi = pub->mpiHead;
    }
    else {
      curMpi->next = newMpi;
      curMpi = curMpi->next;
    }
  }
  pub->mpiCount = mpiCount;
  
	return pub->mpiCount;
}

static uint8_t spgp_read_all_secret_mpis(uint8_t *msg, 
                                         uint32_t *idx,
														 						 uint32_t length, 
                                         spgp_secret_pkt_t *secret) {
  spgp_mpi_t *curMpi;
  spgp_public_pkt_t *pub = (spgp_public_pkt_t*)secret;
  
  if (NULL == msg || NULL == idx || 0 == length || NULL == secret)
  	RAISE(INVALID_ARGS);

	// Set curMpi to last valid Mpi in linked list
	curMpi = pub->mpiHead;
  while (curMpi->next) curMpi = curMpi->next;

  // Read all the MPIs
	if (pub->asymAlgo == ASYM_ALGO_DSA) {
  	// DSA secte MPIs: exponent x
    curMpi->next = spgp_read_mpi(msg, idx, length);
    pub->mpiCount++;
	}
  else {
  	RAISE(FORMAT_UNSUPPORTED);
  }
  
	return pub->mpiCount;
}

static uint32_t spgp_mpi_length(uint8_t *mpi) {
	uint32_t bits;
	if (NULL == mpi) RAISE(INVALID_ARGS);
  bits = ((mpi[0] << 8) | mpi[1]);
  return (bits+7)/8;  
}

static spgp_mpi_t *spgp_read_mpi(uint8_t *msg, uint32_t *idx,
														 uint32_t length) {
	spgp_mpi_t *mpi = NULL;
  
  if (NULL == msg || NULL == idx || 0 == length) RAISE(INVALID_ARGS);
  
  mpi = malloc(sizeof(*mpi));
  if (NULL == mpi) RAISE(OUT_OF_MEMORY);
  memset(mpi, 0, sizeof(*mpi));
  
  // First two bytes are big-endian count of bits in MPI
  if (length - *idx < 2) RAISE(BUFFER_OVERFLOW);
  mpi->bits = ((msg[*idx] << 8) | msg[*idx + 1]);
  
  mpi->count = (mpi->bits+7)/8;
  LOG_PRINT("MPI Bits: %u\n", mpi->bits);
  
  // Allocate space for MPI data
  mpi->data = malloc(mpi->count + 2);
  if (NULL == mpi->data) RAISE(OUT_OF_MEMORY);
  
  // Copy data from input buffer to mpi buffer
  memcpy(mpi->data, msg+*idx, mpi->count + 2);
  *idx += mpi->count + 1;
  
  return mpi;
}

