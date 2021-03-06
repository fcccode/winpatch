/*
 * winpatch - Windows system file patcher
 *
 * Copyright © 2020 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <accctrl.h>
#include <aclapi.h>
#include <imagehlp.h>

#include "msapi_utf8.h"

#pragma comment(lib, "imagehlp.lib")

#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)

#define safe_free(p) do {free((void*)p); p = NULL;} while(0)
#define safe_min(a, b) min((size_t)(a), (size_t)(b))
#define safe_strcp(dst, dst_max, src, count) do {memcpy(dst, src, safe_min(count, dst_max)); \
	((char*)dst)[safe_min(count, dst_max)-1] = 0;} while(0)
#define safe_strcpy(dst, dst_max, src) safe_strcp(dst, dst_max, src, safe_strlen(src)+1)
#define static_strcpy(dst, src) safe_strcpy(dst, sizeof(dst), src)
#define safe_strcat(dst, dst_max, src) safe_strncat(dst, dst_max, src, safe_strlen(src)+1)
#define static_strcat(dst, src) safe_strcat(dst, sizeof(dst), src)
#define safe_closehandle(h) do {if ((h != INVALID_HANDLE_VALUE) && (h != NULL)) {CloseHandle(h); h = INVALID_HANDLE_VALUE;}} while(0)
#define safe_strlen(str) ((((char*)str)==NULL)?0:strlen(str))

#ifndef APP_VERSION
#define APP_VERSION_STR "[DEV]"
#else
#define APP_VERSION_STR STRINGIFY(APP_VERSION)
#endif

extern BOOL SelfSignFile(LPCSTR szFileName, LPCSTR szCertSubject);

static DWORD ReadRegistryKey32(HKEY root, const char* key_name)
{
	char long_key_name[MAX_PATH] = { 0 };
	DWORD val = 0;
	HKEY hApp = NULL;
	size_t i;
	LONG s;
	DWORD dwType = -1, dwSize = sizeof(DWORD);

	if (key_name == NULL)
		return 0;

	for (i = safe_strlen(key_name); i > 0; i--) {
		if (key_name[i] == '\\')
			break;
	}

	if (i >= sizeof(long_key_name))
		return 0;

	static_strcpy(long_key_name, key_name);
	long_key_name[i++] = 0;

	if (RegOpenKeyExA(root, long_key_name, 0, KEY_READ, &hApp) != ERROR_SUCCESS)
		return 0;

	s = RegQueryValueExA(hApp, &key_name[i], NULL, &dwType, (LPBYTE)&val, &dwSize);
	if ((s != ERROR_SUCCESS) || (dwType != REG_DWORD) || (dwSize == 0))
		val = 0;

	RegCloseKey(hApp);
	return val;
}


static __inline char* appname(const char* path)
{
	static char appname[128];
	_splitpath_s(path, NULL, 0, NULL, 0, appname, sizeof(appname), NULL, 0);
	return appname;
}

/*
 * Returns true if:
 * 1. The OS supports UAC, UAC is on, and the current process runs elevated, or
 * 2. The OS doesn't support UAC or UAC is off, and the process is being run by a member of the admin group
 */
BOOL IsCurrentProcessElevated(void)
{
	BOOL r = FALSE;
	DWORD size;
	HANDLE token = INVALID_HANDLE_VALUE;
	TOKEN_ELEVATION te;
	SID_IDENTIFIER_AUTHORITY auth = { SECURITY_NT_AUTHORITY };
	PSID psid;

	if (ReadRegistryKey32(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\\EnableLUA") == 1) {
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
			fprintf(stderr, "Could not get current process token: Error %u\n", GetLastError());
			goto out;
		}
		if (!GetTokenInformation(token, TokenElevation, &te, sizeof(te), &size)) {
			fprintf(stderr, "Could not get token information: Error %u\n", GetLastError());
			goto out;
		}
		r = (te.TokenIsElevated != 0);
	} else {
		if (!AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
			DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &psid))
			goto out;
		if (!CheckTokenMembership(NULL, psid, &r))
			r = FALSE;
		FreeSid(psid);
	}

out:
	safe_closehandle(token);
	return r;
}

/*
 * https://docs.microsoft.com/en-us/windows/win32/secauthz/enabling-and-disabling-privileges-in-c--
 */
static BOOL SetPrivilege(
	HANDLE hToken,              // access token handle
	const char* lpszPrivilege,  // name of privilege to enable/disable
	BOOL bEnablePrivilege       // to enable or disable privilege
)
{
	TOKEN_PRIVILEGES tp;
	LUID luid;

	if (!LookupPrivilegeValueA(
		NULL,            // lookup privilege on local system
		lpszPrivilege,   // privilege to lookup 
		&luid))        // receives LUID of privilege
	{
		fprintf(stderr, "LookupPrivilegeValue error: %u\n", GetLastError());
		return FALSE;
	}

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	if (bEnablePrivilege)
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	else
		tp.Privileges[0].Attributes = 0;

	// Enable the privilege or disable all privileges.
	if (!AdjustTokenPrivileges(
		hToken,
		FALSE,
		&tp,
		sizeof(TOKEN_PRIVILEGES),
		(PTOKEN_PRIVILEGES)NULL,
		(PDWORD)NULL)) {
		fprintf(stderr, "AdjustTokenPrivileges error: %u\n", GetLastError());
		return FALSE;
	}

	if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
		fprintf(stderr, "The token does not have the specified privilege.\n");
		return FALSE;
	}

	return TRUE;
}

/*
 * https://docs.microsoft.com/en-us/windows/win32/secauthz/taking-object-ownership-in-c--
 */
static BOOL TakeOwnership(const char* filename)
{
	BOOL bRetval = FALSE;
	HANDLE hToken = NULL;
	PSID pSIDAdmin = NULL;
	PSID pSIDEveryone = NULL;
	PACL pACL = NULL;
	SID_IDENTIFIER_AUTHORITY SIDAuthWorld = SECURITY_WORLD_SID_AUTHORITY;
	SID_IDENTIFIER_AUTHORITY SIDAuthNT = SECURITY_NT_AUTHORITY;
	EXPLICIT_ACCESS ea[2];
	DWORD dwRes;
	wchar_t* pszFilename = utf8_to_wchar(filename);

	if (pszFilename == NULL) {
		fprintf(stderr, "Could not convert filename '%s'\n", filename);
		goto Cleanup;
	}

	// Specify the DACL to use.
	// Create a SID for the Everyone group.
	if (!AllocateAndInitializeSid(&SIDAuthWorld, 1,
		SECURITY_WORLD_RID,
		0,
		0, 0, 0, 0, 0, 0,
		&pSIDEveryone)) {
		fprintf(stderr, "AllocateAndInitializeSid (Everyone): Error %u\n", GetLastError());
		goto Cleanup;
	}

	// Create a SID for the BUILTIN\Administrators group.
	if (!AllocateAndInitializeSid(&SIDAuthNT, 2,
		SECURITY_BUILTIN_DOMAIN_RID,
		DOMAIN_ALIAS_RID_ADMINS,
		0, 0, 0, 0, 0, 0,
		&pSIDAdmin)) {
		fprintf(stderr, "AllocateAndInitializeSid (Admin): Error %u\n", GetLastError());
		goto Cleanup;
	}

	ZeroMemory(&ea, 2 * sizeof(EXPLICIT_ACCESS));

	// Set read access for Everyone.
	ea[0].grfAccessPermissions = GENERIC_READ;
	ea[0].grfAccessMode = SET_ACCESS;
	ea[0].grfInheritance = NO_INHERITANCE;
	ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	ea[0].Trustee.ptstrName = (LPTSTR)pSIDEveryone;

	// Set full control for Administrators.
	ea[1].grfAccessPermissions = GENERIC_ALL;
	ea[1].grfAccessMode = SET_ACCESS;
	ea[1].grfInheritance = NO_INHERITANCE;
	ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	ea[1].Trustee.ptstrName = (LPTSTR)pSIDAdmin;

	if (SetEntriesInAcl(2, ea, NULL, &pACL) != ERROR_SUCCESS) {
		fprintf(stderr, "Failed SetEntriesInAcl\n");
		goto Cleanup;
	}

	// Try to modify the object's DACL.
	dwRes = SetNamedSecurityInfoW(
		pszFilename,                 // name of the object
		SE_FILE_OBJECT,              // type of object
		DACL_SECURITY_INFORMATION,   // change only the object's DACL
		NULL, NULL,                  // do not change owner or group
		pACL,                        // DACL specified
		NULL);                       // do not change SACL

	if (dwRes == ERROR_SUCCESS) {
		bRetval = TRUE;
		// No more processing needed.
		goto Cleanup;
	}
	if (dwRes != ERROR_ACCESS_DENIED) {
		printf("First SetNamedSecurityInfo call failed: %u\n", dwRes);
		goto Cleanup;
	}

	// If the preceding call failed because access was denied, 
	// enable the SE_TAKE_OWNERSHIP_NAME privilege, create a SID for 
	// the Administrators group, take ownership of the object, and 
	// disable the privilege. Then try again to set the object's DACL.

	// Open a handle to the access token for the calling process.
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken)) {
		fprintf(stderr, "OpenProcessToken failed: Error %u\n", GetLastError());
		goto Cleanup;
	}

	// Enable the SE_TAKE_OWNERSHIP_NAME privilege.
	if (!SetPrivilege(hToken, "SeTakeOwnershipPrivilege", TRUE)) {
		fprintf(stderr, "You must be logged on as Administrator.\n");
		goto Cleanup;
	}

	// Set the owner in the object's security descriptor.
	dwRes = SetNamedSecurityInfoW(
		pszFilename,                 // name of the object
		SE_FILE_OBJECT,              // type of object
		OWNER_SECURITY_INFORMATION,  // change only the object's owner
		pSIDAdmin,                   // SID of Administrator group
		NULL,
		NULL,
		NULL);

	if (dwRes != ERROR_SUCCESS) {
		fprintf(stderr, "Could not set owner: Error %u\n", dwRes);
		goto Cleanup;
	}

	// Disable the SE_TAKE_OWNERSHIP_NAME privilege.
	if (!SetPrivilege(hToken, "SeTakeOwnershipPrivilege", FALSE)) {
		fprintf(stderr, "SetPrivilege call failed unexpectedly.\n");
		goto Cleanup;
	}

	// Try again to modify the object's DACL, now that we are the owner.
	dwRes = SetNamedSecurityInfoW(
		(LPWSTR)pszFilename,         // name of the object
		SE_FILE_OBJECT,              // type of object
		DACL_SECURITY_INFORMATION,   // change only the object's DACL
		NULL, NULL,                  // do not change owner or group
		pACL,                        // DACL specified
		NULL);                       // do not change SACL

	if (dwRes == ERROR_SUCCESS)
		bRetval = TRUE;
	else
		fprintf(stderr, "Second SetNamedSecurityInfo call failed: Error %u\n", dwRes);

Cleanup:
	if (pSIDAdmin)
		FreeSid(pSIDAdmin);
	if (pSIDEveryone)
		FreeSid(pSIDEveryone);
	if (pACL)
		LocalFree(pACL);
	if (hToken)
		CloseHandle(hToken);
	free(pszFilename);
	return bRetval;
}

BOOL CreateBackup(const char* path)
{
	BOOL bRet = FALSE;
	struct _stat64 st;
	size_t size = safe_strlen(path) + 5;
	char* backup_path = NULL;

	if (_stat64U(path, &st) != 0)
		return FALSE;

	backup_path = malloc(size);
	if (backup_path == NULL)
		return FALSE;
	strcpy_s(backup_path, size, path);
	strcat_s(backup_path, size, ".bak");
	if (_stat64U(backup_path, &st) == 0) {
		fprintf(stdout, "Backup '%s' already exists - keeping it\n", backup_path);
		free(backup_path);
		return TRUE;
	}
	bRet = CopyFileU(path, backup_path, TRUE);
	if (bRet)
		fprintf(stdout, "Saved backup as '%s'\n", backup_path);
	free(backup_path);
	return bRet;
}

static DWORD RemoveDigitalSignature(const char* filename)
{
	DWORD dwNumCerts = -1;
	HANDLE hFile = NULL;

	hFile = CreateFileU(filename, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (hFile == NULL) {
		fprintf(stderr, "Could not open file to remove digital signature: Error %u\n", GetLastError());
		return -1;
	}

	if (ImageEnumerateCertificates(hFile, CERT_SECTION_TYPE_ANY, &dwNumCerts, NULL, 0)) {
		switch(dwNumCerts) {
		case 0:
			break;
		case 1:
			if (!ImageRemoveCertificate(hFile, 0)) {
				fprintf(stderr, "Could not delete digital signatures: Error %u\n", GetLastError());
				dwNumCerts = -1;
			}
			break;
		default:
			fprintf(stderr, "Unexpected number of signatures!\n");
			dwNumCerts = -1;
			break;
		}
	}

	safe_closehandle(hFile);
	return dwNumCerts;
}

static BOOL UpdateChecksum(const char* filename, DWORD* dwCheckSum)
{
	BOOL bRet = FALSE;
	HANDLE hFile = NULL, hFileMapping = NULL;
	PIMAGE_DOS_HEADER pImageDOSHeader = NULL;
	PIMAGE_NT_HEADERS32 pImageNTHeader32 = NULL;
	PIMAGE_NT_HEADERS64 pImageNTHeader64 = NULL;

	hFile = CreateFileU(filename, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (hFile == NULL) {
		fprintf(stderr, "Could not open file to update checksum: %u\n", GetLastError());
		return FALSE;
	}

	hFileMapping = CreateFileMapping(hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
	if (hFileMapping == NULL) {
		fprintf(stderr, "Could not create file mapping to update checksum: Error %u\n", GetLastError());
		goto out;
	}

	pImageDOSHeader = (PIMAGE_DOS_HEADER)MapViewOfFile(hFileMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (pImageDOSHeader == NULL) {
		fprintf(stderr, "Could not get mapped view address to update checksum: Error %u\n", GetLastError());
		goto out;
	}
	if (pImageDOSHeader->e_magic != IMAGE_DOS_SIGNATURE) {
		fprintf(stderr, "DOS header not found\n");
		goto out;
	}
	pImageNTHeader32 = (PIMAGE_NT_HEADERS32)((uintptr_t)pImageDOSHeader + pImageDOSHeader->e_lfanew);
	if (pImageNTHeader32->Signature != IMAGE_NT_SIGNATURE) {
		fprintf(stderr, "NT header not found\n");
		goto out;
	}

	switch (pImageNTHeader32->FileHeader.Machine) {
	case IMAGE_FILE_MACHINE_IA64:
	case IMAGE_FILE_MACHINE_ALPHA64:
	case IMAGE_FILE_MACHINE_AMD64:
	case IMAGE_FILE_MACHINE_ARM64:
		pImageNTHeader64 = (PIMAGE_NT_HEADERS64)pImageNTHeader32;
		if (pImageNTHeader64->OptionalHeader.CheckSum != dwCheckSum[0]) {
			fprintf(stderr, "Old checksum does not match! Is this a 64-bit executable?");
			goto out;
		}
		pImageNTHeader64->OptionalHeader.CheckSum = dwCheckSum[1];
		fprintf(stdout, "64-bit checksum updated\n");
		break;
	default:
		if (pImageNTHeader32->OptionalHeader.CheckSum != dwCheckSum[0]) {
			fprintf(stderr, "Old checksum does not match! Is this a 32-bit executable?");
			goto out;
		}
		pImageNTHeader32->OptionalHeader.CheckSum = dwCheckSum[1];
		fprintf(stdout, "32-bit checksum updated\n");
		break;
	}
	bRet = TRUE;

out:
	if (pImageDOSHeader != NULL)
		UnmapViewOfFile(pImageDOSHeader);
	safe_closehandle(hFileMapping);
	safe_closehandle(hFile);
	return bRet;
}

static int main_utf8(int argc, char** argv)
{
	FILE* file = NULL;
	DWORD r, dwCheckSum[2];
	int i, patched = 0;
	uint64_t* patch, val, pos;
	char system_dir[128];

	if (!IsCurrentProcessElevated()) {
		fprintf(stderr, "This command must be run from an elevated prompt.\n");
		return -1;
	}

	if (argc < 2) {
		fprintf(stderr, "Usage: %s filename [QWORD QWORD [QWORD QWORD]...].\n", appname(argv[0]));
		fprintf(stderr, "The QWORDs *must* be aligned to 64-bit.\n");
		return -2;
	}

	fprintf(stderr, "%s %s © 2020 Pete Batard <pete@akeo.ie>\n\n",
		appname(argv[0]), APP_VERSION_STR);

	if (GetSystemDirectoryU(system_dir, sizeof(system_dir)) == 0)
		static_strcpy(system_dir, "C:\\Windows\\System32");
	if (_strnicmp(argv[1], system_dir, strlen(system_dir)) == 0) {
		fprintf(stderr, "Patching of active system files is prohibited!\n");
		return -1;
	}

	if (!TakeOwnership(argv[1])) {
		fprintf(stderr, "Could not take ownership of %s\n", argv[1]);
		return -1;
	}

	if (!CreateBackup(argv[1])) {
		fprintf(stderr, "Could not create backup of %s\n", argv[1]);
		return -1;
	}

	if (argc <= 2) {
		fprintf(stderr, "No patch data provided!\n");
		return -1;
	}

	if (argc % 2) {
		fprintf(stderr, "Values must be provided in [ORIGINAL PATCHED] pairs\n");
		return -1;
	}

	// We're not going to win prizes for speed, but who cares...
	file = fopenU(argv[1], "rb+");
	if (file == NULL) {
		fprintf(stderr, "Could not open '%s'\n", argv[1]);
		return -1;
	}

	patch = calloc((size_t)argc - 2, sizeof(uint64_t));
	if (patch == NULL) {
		fprintf(stderr, "calloc error\n");
		return -1;
	}

	for (i = 0; i < argc - 2; i++)
		patch[i] = strtoull(argv[i + 2], NULL, 16);

	for (pos = 0; fread(&val, sizeof(uint64_t), 1, file) == 1; pos += sizeof(uint64_t)) {
		for (i = 0; i < argc - 2; i += 2) {
			if (val == patch[i]) {
				fprintf(stdout, "%08llX: %016llX -> %016llX... ", pos, val, patch[i + 1]);
				fseek(file, -1 * (long)sizeof(uint64_t), SEEK_CUR);
				if (fwrite(&patch[i + 1], sizeof(uint64_t), 1, file) != 1) {
					fprintf(stdout, "ERROR!\n");
				} else {
					fprintf(stdout, "SUCCESS\n");
					patched++;
				}
				fflush(file);
			}
		}
	}
	free(patch);
	fclose(file);
	if (patched == 0) {
		fprintf(stdout, "No elements were patched - aborting\n");
		return 0;
	}

	// Since the whole point is to alter the file, remove the digital signature
	r = RemoveDigitalSignature(argv[1]);
	switch (r) {
	case 0:
		fprintf(stdout, "No digital signature to remove\n");
		break;
	case 1:
		fprintf(stdout, "Removed digital signature\n");
		break;
	default:
		fprintf(stderr, "Could not remove digital signature\n");
		return -1;
	}

	r = MapFileAndCheckSumU(argv[1], &dwCheckSum[0], &dwCheckSum[1]);
	if (r != CHECKSUM_SUCCESS) {
		fprintf(stderr, "Could not compute checksum: %u\n", r);
		return -1;
	}

	fprintf(stdout, "PE Checksum: %08X\n", dwCheckSum[1]);
	if (dwCheckSum[0] != dwCheckSum[1] && !UpdateChecksum(argv[1], dwCheckSum)) {
		fprintf(stderr, "Could not update checksum\n");
		return -1;
	}

	fprintf(stdout, "Applying digital signature...\n");
	if (!SelfSignFile(argv[1], "CN = Test Signing Certificate")) {
		fprintf(stderr, "Could not sign file\n");
		return -1;
	}
	fprintf(stdout, "Successfully patched '%s'\n", argv[1]);

	return patched;
}

int wmain(int argc, wchar_t** argv16)
{
	SetConsoleOutputCP(CP_UTF8);
	char** argv = calloc(argc, sizeof(char*));
	if (argv == NULL)
		return -1;
	for (int i = 0; i < argc; i++)
		argv[i] = wchar_to_utf8(argv16[i]);
	int r = main_utf8(argc, argv);
	for (int i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
#ifdef _DEBUG
	_CrtDumpMemoryLeaks();
#endif
	return r;
}
