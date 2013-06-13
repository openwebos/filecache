/* @@@LICENSE
*
*      Copyright (c) 2007-2013 LG Electronics, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <assert.h>
#include <giomm/init.h>
#include "FileCacheServiceApp.h"
#include "core/MojLogEngine.h"
#include "CacheBase.h"

const char* const ServiceApp::ServiceName = "com.palm.filecache";

int main(int argc, char** argv) {

  // This makes sure using an unsigned long long for
  // cachedObjectId_t will yield 64 bits.  Using uint64_t caused
  // issues with printf formats between 32 and 64 bit machines
  assert(sizeof(cachedObjectId_t) == 8);

  ServiceApp app;
  int mainResult = app.main(argc, argv);

  return mainResult;
}

ServiceApp::ServiceApp() : m_service(true) {

  //  MojLogEngine::instance()->reset(MojLogger::LevelTrace);

  // When creating the service app, walk the directory tree and build
  // the cache data structures for objects already cached.
  m_fileCacheSet = new CFileCacheSet;
  m_fileCacheSet->WalkDirTree();

  // This is part of the fix for NOV-128944.
  m_fileCacheSet->CleanupAtStartup();
}

MojErr ServiceApp::open() {

  Gio::init();

  MojErr err = Base::open();
  MojErrCheck(err);

  err = m_service.open(ServiceName);
  MojErrCheck(err);

  err = m_service.attach(m_reactor.impl());
  MojErrCheck(err);

  m_handler.reset(new CategoryHandler(m_fileCacheSet));
  MojAllocCheck(m_handler.get());

  err = m_handler->RegisterMethods();
  MojErrCheck(err);

  err = m_service.addCategory(MojLunaService::DefaultCategory,
			      m_handler.get());
  MojErrCheck(err);

#if !defined(TARGET_DESKTOP)
  char* upstartJob = ::getenv("UPSTART_JOB");
  if (upstartJob) {
    char* upstartEvent = g_strdup_printf("%s emit %s-ready",
    					 (gchar *)s_InitctlCommand.c_str(),
    					 upstartJob);
    if (upstartEvent) {
      int retVal = ::system(upstartEvent);
      if (retVal == -1) {
	MojLogError(s_globalLogger,
		    _T("ServiceApp: Failed to emit upstart event"));
      }
      g_free(upstartEvent);
    } else {
      MojLogError(s_globalLogger,
		  _T("ServiceApp: Failed to allocate memory for upstart emit"));
    }
  }
#endif // #if !defined(TARGET_DESKTOP)

  return MojErrNone;
}
