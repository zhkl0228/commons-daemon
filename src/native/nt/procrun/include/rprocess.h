/* Copyright 2000-2004 The Apache Software Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#ifndef _RPROCESS_H_INCLUDED_
#define _RPROCESS_H_INCLUDED_

__APXBEGIN_DECLS

BOOL        apxProcessExecute(APXHANDLE hProcess);

APXHANDLE   apxCreateProcessA(APXHANDLE hPool, DWORD dwOptions,
                              LPAPXFNCALLBACK fnCallback,
                              LPCSTR szUsername, LPCSTR szPassword,
                              BOOL bLogonAsService);
APXHANDLE   apxCreateProcessW(APXHANDLE hPool, DWORD dwOptions,
                              LPAPXFNCALLBACK fnCallback,
                              LPCWSTR szUsername, LPCWSTR szPassword,
                              BOOL bLogonAsService);

#ifdef _UNICODE
#define apxCreateProcess    apxCreateProcessW
#else
#define apxCreateProcess    apxCreateProcessA
#endif

BOOL        apxProcessSetExecutableA(APXHANDLE hProcess, LPCSTR szName);
BOOL        apxProcessSetExecutableW(APXHANDLE hProcess, LPCWSTR szName);

BOOL        apxProcessSetCommandLineA(APXHANDLE hProcess, LPCSTR szCmdline);
BOOL        apxProcessSetCommandLineW(APXHANDLE hProcess, LPCWSTR szCmdline);
BOOL        apxProcessSetCommandArgsW(APXHANDLE hProcess, LPCWSTR szTitle,
                                      DWORD dwArgc, LPCWSTR *lpArgs);

BOOL        apxProcessSetWorkingPathA(APXHANDLE hProcess, LPCSTR szPath);
BOOL        apxProcessSetWorkingPathW(APXHANDLE hProcess, LPCWSTR szPath);

DWORD       apxProcessPutcA(APXHANDLE hProcess, INT ch);
DWORD       apxProcessPutcW(APXHANDLE hProcess, INT ch);
DWORD       apxProcessPutsA(APXHANDLE hProcess, LPCSTR szString);
DWORD       apxProcessPutsW(APXHANDLE hProcess, LPCWSTR szString);

#ifndef _UNICODE
#define     apxProcessPutc  apxProcessPutcA
#define     apxProcessPuts  apxProcessPutsA
#else
#define     apxProcessPutc  apxProcessPutcW
#define     apxProcessPuts  apxProcessPutsW
#endif

DWORD       apxProcessWrite(APXHANDLE hProcess, LPCVOID lpData, DWORD dwLen);

VOID        apxProcessCloseInputStream(APXHANDLE hProcess);
BOOL        apxProcessFlushStdin(APXHANDLE hProcess);

DWORD       apxProcessWait(APXHANDLE hProcess, DWORD dwMilliseconds,
                           BOOL bKill);

BOOL        apxProcessRunning(APXHANDLE hProcess);


__APXEND_DECLS

#endif /* _RPROCESS_H_INCLUDED_ */