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

#include "CategoryHandler.h"
#include "FileCacheError.h"
#include "AsyncFileCopier.h"
#include "CacheBase.h"

#include "sandbox.h"
#include "boost/filesystem.hpp"
#include <sstream>

namespace fs = boost::filesystem;

MojLogger CategoryHandler::s_log(_T("filecache.categoryhandler"));

const CategoryHandler::Method CategoryHandler::s_privMethods[] = {
  {_T("DefineType"), (Callback) &CategoryHandler::DefineType},
  {_T("ChangeType"), (Callback) &CategoryHandler::ChangeType},
  {_T("DeleteType"), (Callback) &CategoryHandler::DeleteType},
  {_T("CopyCacheObject"), (Callback) &CategoryHandler::CopyCacheObject},
  {NULL, NULL}
};

const CategoryHandler::Method CategoryHandler::s_pubMethods[] = {
  {_T("DescribeType"), (Callback) &CategoryHandler::DescribeType},
  {_T("InsertCacheObject"), (Callback) &CategoryHandler::InsertCacheObject},
  {_T("ResizeCacheObject"), (Callback) &CategoryHandler::ResizeCacheObject},
  {_T("ExpireCacheObject"), (Callback) &CategoryHandler::ExpireCacheObject},
  {_T("SubscribeCacheObject"), (Callback) &CategoryHandler::SubscribeCacheObject},
  {_T("TouchCacheObject"), (Callback) &CategoryHandler::TouchCacheObject},
  {_T("GetCacheStatus"), (Callback) &CategoryHandler::GetCacheStatus},
  {_T("GetCacheTypeStatus"), (Callback) &CategoryHandler::GetCacheTypeStatus},
  {_T("GetCacheObjectSize"), (Callback) &CategoryHandler::GetCacheObjectSize},
  {_T("GetCacheObjectFilename"), (Callback) &CategoryHandler::GetCacheObjectFilename},
  {_T("GetCacheTypes"), (Callback) &CategoryHandler::GetCacheTypes},
  {_T("GetVersion"), (Callback) &CategoryHandler::GetVersion},
  {NULL, NULL}
};

CategoryHandler::CategoryHandler(CFileCacheSet* cacheSet)
  : m_fileCacheSet(cacheSet) {

  MojLogTrace(s_log);

  SetupWorkerTimer();
}

CategoryHandler::~CategoryHandler() {

  MojLogTrace(s_log);
}

MojErr
CategoryHandler::RegisterMethods() {

  MojLogTrace(s_log);

  MojErr err = addMethods(s_privMethods, false);
  MojErrCheck(err);
  err = addMethods(s_pubMethods, true);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("RegisterMethods: Registered all service methods."));

  return MojErrNone;
}

MojErr
CategoryHandler::DefineType(MojServiceMessage* msg, MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString typeName;
  MojInt64 loWatermark = 0;
  MojInt64 hiWatermark = 0;
  MojInt64 size = 0;
  MojInt64 cost = 0;
  MojInt64 lifetime = 0;
  bool dirType = false;

  err = payload.getRequired(_T("typeName"), typeName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("DefineType: new type '%s' to be defined."),
	      typeName.data());

  err = payload.getRequired(_T("loWatermark"), loWatermark);
  MojErrCheck(err);
  err = payload.getRequired(_T("hiWatermark"), hiWatermark);
  MojErrCheck(err);
  payload.get(_T("size"), size);
  payload.get(_T("cost"), cost);
  payload.get(_T("lifetime"), lifetime);
  payload.get(_T("dirType"), dirType);

  std::string msgText;
  if (typeName.length() > 64) {
    msgText = "DefineType: Invalid params: typeName must be 64 characters or less.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if (typeName.startsWith(_T("."))) {
    msgText = "DefineType: Invalid params: typeName must not start with a '.'.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if (size < 0) {
    msgText = "DefineType: Invalid params: size must not be negative.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if ((cost < 0) || (cost > 100)) {
    msgText = "DefineType: Invalid params: cost must be in the range of 0 to 100.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if (lifetime < 0) {
    msgText = "DefineType: Invalid params: lifetime must not be negative.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if (loWatermark <= 0) {
    msgText = "DefineType: Invalid params: loWatermark must be greater than 0.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if (hiWatermark <= loWatermark) {
    msgText = "DefineType: Invalid params: hiWatermark must be greater than loWatermark.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if (dirType && reinterpret_cast<MojLunaMessage*>(msg)->isPublic()) {
    msgText = "DefineType: Invalid params: specifying dirType not authorized.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  }
  if (!msgText.empty()) {
    err = msg->replyError((MojErr) FCInvalidParams, msgText.c_str());
  } else {
    MojLogDebug(s_log, _T("DefineType: params: loWatermark = '%lld', hiWatermark = '%lld',"),
		loWatermark, hiWatermark);
    MojLogDebug(s_log, _T("DefineType: params: size = '%lld', cost = '%lld', lifetime = '%lld'."),
		size, cost, lifetime);

    CCacheParamValues params((cacheSize_t) loWatermark,
			     (cacheSize_t) hiWatermark,
			     (cacheSize_t) size, (paramValue_t) cost,
			     (paramValue_t) lifetime);

    if (m_fileCacheSet->TypeExists(std::string(typeName.data()))) {
      msgText = "DefineType: Type '";
      msgText += typeName.data();
      msgText += "' ";
#ifdef NEEDS_CONFIGURATOR_FIX
      CCacheParamValues curParams =
	m_fileCacheSet->DescribeType(std::string(typeName.data()));
      if (params != curParams) {
	MojLogError(s_log,
		    _T("DefineType: cur params: loWatermark = '%lld', hiWatermark = '%lld',"),
		    (long long int) curParams.GetLoWatermark(),
		    (long long int) curParams.GetHiWatermark());
	MojLogError(s_log,
		    _T("DefineType: cur params: size = '%lld', cost = '%lld', lifetime = '%lld'."),
		    (long long int) curParams.GetSize(),
		    (long long int) curParams.GetCost(),
		    (long long int) curParams.GetLifetime());
	MojLogError(s_log,
		    _T("DefineType: new params: loWatermark = '%lld', hiWatermark = '%lld',"),
		    (long long int) params.GetLoWatermark(),
		    (long long int) params.GetHiWatermark());
	MojLogError(s_log,
		    _T("DefineType: new params: size = '%lld', cost = '%lld', lifetime = '%lld'."),
		    (long long int) params.GetSize(),
		    (long long int) params.GetCost(),
		    (long long int) params.GetLifetime());
	msgText += "has different configuration.";
	err = msg->replyError((MojErr) FCConfigurationError, msgText.c_str());
      } else {
#endif
	msgText += "already exists.";
	err = msg->replyError((MojErr) FCExistsError, msgText.c_str());
#ifdef NEEDS_CONFIGURATOR_FIX
      }
#endif
    } else {
      if (m_fileCacheSet->DefineType(msgText, std::string(typeName.data()),
				     &params, dirType)) {
	err = msg->replySuccess();
      } else {
	err = msg->replyError((MojErr) FCDefineError, msgText.c_str());
      }
    }
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::ChangeType(MojServiceMessage* msg, MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString typeName;
  MojInt64 loWatermark = 0;
  MojInt64 hiWatermark = 0;
  MojInt64 size = 0;
  MojInt64 cost = 0;
  MojInt64 lifetime = 0;

  err = payload.getRequired(_T("typeName"), typeName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("ChangeType: existing type '%s' to be changed."),
	      typeName.data());

  payload.get(_T("loWatermark"), loWatermark);
  payload.get(_T("hiWatermark"), hiWatermark);
  payload.get(_T("size"), size);
  payload.get(_T("cost"), cost);
  payload.get(_T("lifetime"), lifetime);

  std::string msgText;
  if (size < 0) {
    msgText = "ChangeType: Invalid params: size must not be negative.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if ((cost < 0) || (cost > 100)) {
    msgText = "ChangeType: Invalid params: cost must be in the range of 0 to 100.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if (lifetime < 0) {
    msgText = "ChangeType: Invalid params: lifetime must not be negative.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if (loWatermark < 0) {
    msgText = "ChangeType: Invalid params: loWatermark must be greater than 0.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else if ((hiWatermark != 0) && (hiWatermark <= loWatermark)) {
    msgText = "ChangeType: Invalid params: hiWatermark must be greater than loWatermark.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
  }
  if (!msgText.empty()) {
    err = msg->replyError((MojErr) FCInvalidParams, msgText.c_str());
  } else {
    MojLogDebug(s_log, _T("ChangeType: params: loWatermark = '%lld', hiWatermark = '%lld',"),
		loWatermark, hiWatermark);
    MojLogDebug(s_log, _T("ChangeType: params: size = '%lld', cost = '%lld', lifetime = '%lld'."),
		size, cost, lifetime);

    CCacheParamValues params((cacheSize_t) loWatermark,
			     (cacheSize_t) hiWatermark,
			     (cacheSize_t) size, (paramValue_t) cost,
			     (paramValue_t) lifetime);

    if (m_fileCacheSet->ChangeType(msgText, std::string(typeName.data()),
				   &params)) {
      err = msg->replySuccess();
    } else {
      err = msg->replyError((MojErr) FCChangeError, msgText.c_str());
    }
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::DeleteType(MojServiceMessage* msg, MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString typeName;
  MojInt64 freedSpace = 0;

  err = payload.getRequired(_T("typeName"), typeName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("DeleteType: existing type '%s' to be deleted."),
	      typeName.data());

  std::string msgText;
  freedSpace = m_fileCacheSet->DeleteType(msgText, std::string(typeName.data()));

  if (freedSpace >= 0) {
    MojLogDebug(s_log, _T("DeleteType: deleting type '%s' freed '%lld' bytes."),
		typeName.data(), freedSpace);
    MojObject reply;
    err = reply.putInt(_T("freedSpace"), freedSpace);
    MojErrCheck(err);
    err = msg->replySuccess(reply);
  } else {
    err = msg->replyError((MojErr) FCDeleteError, msgText.c_str());
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::DescribeType(MojServiceMessage* msg,
			      MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString typeName;

  err = payload.getRequired(_T("typeName"), typeName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("DescribeType: existing type '%s' to be queried."),
	      typeName.data());

  if (m_fileCacheSet->TypeExists(std::string(typeName.data()))) {
    CCacheParamValues params =
      m_fileCacheSet->DescribeType(std::string(typeName.data()));

    MojLogDebug(s_log, _T("DescribeType: params: loWatermark = '%d', hiWatermark = '%d',"),
		params.GetLoWatermark(), params.GetHiWatermark());
    MojLogDebug(s_log, _T("DescribeType: params: size = '%d', cost = '%d', lifetime = '%d'."),
		params.GetSize(), params.GetCost(), params.GetLifetime());

    MojObject reply;
    err = reply.putInt(_T("loWatermark"), (MojInt64) params.GetLoWatermark());
    MojErrCheck(err);
    err = reply.putInt(_T("hiWatermark"), (MojInt64) params.GetHiWatermark());
    MojErrCheck(err);
    err = reply.putInt(_T("size"), (MojInt64) params.GetSize());
    MojErrCheck(err);
    err = reply.putInt(_T("cost"), (MojInt64) params.GetCost());
    MojErrCheck(err);
    err = reply.putInt(_T("lifetime"), (MojInt64) params.GetLifetime());
    MojErrCheck(err);
    err = msg->replySuccess(reply);
  } else {
    std::string msgText("DescribeType: Type '");
    msgText += typeName.data();
    msgText += "' does not exists.";
    err = msg->replyError((MojErr) FCExistsError, msgText.c_str());
  }

  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::InsertCacheObject(MojServiceMessage* msg,
				   MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString typeName, fileName;
  MojInt64 size = 0;
  MojInt64 cost = 0;
  MojInt64 lifetime = 0;

  err = payload.getRequired(_T("typeName"), typeName);
  MojErrCheck(err);
  err = payload.getRequired(_T("fileName"), fileName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("InsertCacheObject: inserting object into type '%s' for file '%s',"),
	      typeName.data(), fileName.data());

  payload.get(_T("size"), size);
  payload.get(_T("cost"), cost);
  payload.get(_T("lifetime"), lifetime);
  MojLogDebug(s_log,
	      _T("InsertCacheObject: params: size = '%lld', cost = '%lld', lifetime = '%lld'."),
	      size, cost, lifetime);

  std::string msgText;
  if (m_fileCacheSet->TypeExists(std::string(typeName.data()))) {
    CCacheParamValues params =
      m_fileCacheSet->DescribeType(std::string(typeName.data()));
	// if needed, overwrite values with defaults
    if (size == 0) size = params.GetSize();
    if (cost == 0) cost = params.GetCost();
    if (lifetime == 0) lifetime = params.GetLifetime();
    if (size <= 0) {
      msgText = "InsertCacheObject: Invalid params: size must be greater than 0.";
    } else if ((size <= GetFilesystemFileSize(1)) &&
	       m_fileCacheSet->isTypeDirType(typeName.data())) {
      msgText = "InsertCacheObject: Invalid params: size must be greater than 1 block when dirType = true.";
    } else if ((cost < 0) || (cost > 100)) {
      msgText = "InsertCacheObject: Invalid params: cost must be in the range of 0 to 100.";
    } else if (lifetime < 0) {
      msgText = "InsertCacheObject: Invalid params: lifetime must not be negative.";
    } else if (fileName.find(_T("/")) != MojInvalidIndex) {
      msgText = "InsertCacheObject: Invalid params: fileName must not contain a '/'.";
    }
  } else {
    msgText = "InsertCacheObject: No type '" + std::string(typeName.data())
      + "' defined.";
  }
  if (!msgText.empty()) {
    MojLogError(s_log, _T("%s"), msgText.c_str());
    err = msg->replyError((MojErr) FCInvalidParams, msgText.c_str());
  } else {
    cachedObjectId_t objId =
      m_fileCacheSet->InsertCacheObject(msgText, std::string(typeName.data()),
					std::string(fileName.data()),
					(cacheSize_t) size,
					(paramValue_t) cost,
					(paramValue_t) lifetime);

    MojLogDebug(s_log, _T("InsertCacheObject: new object id = %llu."), objId);
    if (objId > 0) {
      bool subscribed = false;
      MojString pathName;
      MojObject reply;
      if (payload.get(_T("subscribe"), subscribed) && subscribed) {
	const std::string fpath(m_fileCacheSet->SubscribeCacheObject(msgText, objId));
	if (!fpath.empty()) {
	  err = pathName.assign(fpath.c_str());
	  MojErrCheck(err);
	  MojRefCountedPtr<Subscription> cancelHandler(new Subscription(*this,
									msg,
									pathName));
	  MojAllocCheck(cancelHandler.get());
	  m_subscribers.push_back(cancelHandler.get());
	  MojLogDebug(s_log, _T("InsertCacheObject: subscribed new object '%s'."),
		      fpath.c_str());
	  err = reply.putBool(_T("subscribed"), true);
	  MojErrCheck(err);
	} else if (!msgText.empty()) {
	  msgText = "SubscribeCacheObject: " + msgText;
	  MojLogError(s_log, _T("%s"), msgText.c_str());
	}
      } else {
	const std::string dirBase(m_fileCacheSet->GetBaseDirName());
	err = pathName.assign(BuildPathname(objId, dirBase,
					    std::string(typeName.data()),
					    std::string(fileName.data())).c_str());
	MojErrCheck(err);
      }
      err = reply.putString(_T("pathName"), pathName);
      MojErrCheck(err);

      err = msg->replySuccess(reply);
    } else {
      err = msg->replyError((MojErr) FCExistsError, msgText.c_str());
    }
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::ResizeCacheObject(MojServiceMessage* msg,
				   MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString pathName;
  MojInt64 newSize;

  err = payload.getRequired(_T("pathName"), pathName);
  MojErrCheck(err);

  err = payload.getRequired(_T("newSize"), newSize);
  MojErrCheck(err);

  std::string msgText;
  if (newSize <= 0) {
    msgText = "ResizeCacheObject: Invalid params: size must be greater than 0.";
    MojLogError(s_log, _T("%s"), msgText.c_str());
    err = msg->replyError((MojErr) FCInvalidParams, msgText.c_str());
  } else {
    MojLogDebug(s_log, _T("ResizeCacheObject: resizing file '%s' to '%lld'."),
		pathName.data(), newSize);

    cacheSize_t size = -1;
    const cachedObjectId_t objId = GetObjectIdFromPath(pathName.data());
    MojLogDebug(s_log, _T("ResizeCacheObject: file '%s' produced object id '%llu'."),
		pathName.data(), objId);
    FCErr errCode = FCErrorNone;
    if (objId > 0) {
      if (GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
			      pathName.data()) ==
	  m_fileCacheSet->GetTypeForObjectId(objId)) {
	size = m_fileCacheSet->Resize(objId, (cacheSize_t) newSize);
	MojLogDebug(s_log, _T("ResizeCacheObject: final size is '%d'."), size);

	if (size == (cacheSize_t) newSize) {
	  MojObject reply;
	  err = reply.putInt(_T("newSize"), (MojInt64) size);
	  MojErrCheck(err);
	  err = msg->replySuccess(reply);
	  MojErrCheck(err);
	} else {
	  msgText = "ResizeCacheObject: Unable to resize object.";
	  errCode = FCResizeError;
	}
      } else {
	msgText = "ResizeCacheObject: pathName no longer found in cache.";
	errCode = FCExistsError;
	MojLogError(s_log, _T("%s"), msgText.c_str());
      }
    } else {
      msgText = "ResizeCacheObject: Invalid object id derived from pathname.";
      errCode = FCExistsError;
      MojLogError(s_log, _T("%s"), msgText.c_str());
    }

    if (!msgText.empty()) {
      err = msg->replyError((MojErr) errCode, msgText.c_str());
    }
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::ExpireCacheObject(MojServiceMessage* msg,
				   MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString pathName;
  std::string msgText;

  err = payload.getRequired(_T("pathName"), pathName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("ExpireCacheObject: expiring object '%s'."),
	      pathName.data());

  FCErr errCode = FCErrorNone;
  const cachedObjectId_t objId = GetObjectIdFromPath(pathName.data());
  if (objId > 0) {
    if (GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
			    pathName.data()) ==
	m_fileCacheSet->GetTypeForObjectId(objId)) {
      if (m_fileCacheSet->ExpireCacheObject(objId)) {
	MojLogWarning(s_log,
		      _T("ExpireCacheObject: Object '%s' expired by user '%s'."),
		      pathName.data(), (CallerID(msg)).c_str());
      } else {
	msgText = "ExpireCacheObject: Expire deferred, object in use.";
	errCode = FCInUseError;
      }
    } else {
      MojLogError(s_log,
		  _T("GetTypeFromPath = %s, GetTypeForObjectId = %s, objId = %llu"),
		  GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
				      pathName.data()).c_str(),
		  m_fileCacheSet->GetTypeForObjectId(objId).c_str(), objId);

      msgText = "ExpireCacheObject: pathName no longer found in cache.";
      MojLogError(s_log, _T("%s"), msgText.c_str());
      if (GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
			      pathName.data()).empty()) {
	errCode = FCExistsError;
      } else {
	msgText.clear();
      }
    }
  } else {
      msgText = "ExpireCacheObject: Invalid object id derived from pathname.";
      errCode = FCExistsError;
      MojLogError(s_log, _T("%s"), msgText.c_str());
  }

  if (!msgText.empty()) {
    err = msg->replyError((MojErr) errCode, msgText.c_str());
  } else {
    err = msg->replySuccess();
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::SubscribeCacheObject(MojServiceMessage* msg,
				      MojObject& payload) {

  MojLogTrace(s_log);

  MojString pathName;
  MojErr err = payload.getRequired(_T("pathName"), pathName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("SubscribeCacheObject: subscribing to file '%s'."),
	      pathName.data());
  std::string msgText;

  const cachedObjectId_t objId = GetObjectIdFromPath(pathName.data());
  if (objId > 0) {
    bool subscribed = false;
    if (payload.get(_T("subscribe"), subscribed) && subscribed) {
      if (GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
			      pathName.data()) ==
	  m_fileCacheSet->GetTypeForObjectId(objId)) {
	const std::string fpath(m_fileCacheSet->SubscribeCacheObject(msgText, objId));
	if (!fpath.empty()) {
	  MojObject reply;
	  MojRefCountedPtr<Subscription> cancelHandler(new Subscription(*this,
									msg,
									pathName));
	  MojAllocCheck(cancelHandler.get());
	  m_subscribers.push_back(cancelHandler.get());
	  MojLogDebug(s_log, _T("SubscribeCacheObject: subscribed object '%s'."),
		      fpath.c_str());
	  err = reply.putBool(_T("subscribed"), true);
	  MojErrCheck(err);
	  err = msg->replySuccess(reply);
	} else if (!msgText.empty()) {
	  msgText = "SubscribeCacheObject: " + msgText;
	  MojLogError(s_log, _T("%s"), msgText.c_str());
	} else {
	  msgText = "SubscribeCacheObject: Could not find object to match derived id.";
	  MojLogError(s_log, _T("%s"), msgText.c_str());
	}
      } else {
	msgText = "SubscribeCacheObject: pathName no longer found in cache.";
	MojLogError(s_log, _T("%s"), msgText.c_str());
      }
    }
  } else {
      msgText = "SubscribeCacheObject: Invalid object id derived from pathname.";
      MojLogError(s_log, _T("%s"), msgText.c_str());
  }

  if (!msgText.empty()) {
    err = msg->replyError((MojErr) FCExistsError, msgText.c_str());
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::CancelSubscription(Subscription* sub,
				    MojServiceMessage* msg,
				    MojString& pathName) {

  MojLogTrace(s_log);

  const cachedObjectId_t objId = GetObjectIdFromPath(pathName.data());
  if (objId > 0) {
    const std::string typeName(GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
						   pathName.data()));
    if (!typeName.empty()) {
      m_fileCacheSet->UnSubscribeCacheObject(typeName, objId);
    } else {
      MojLogError(s_log,
		  _T("CancelSubscription: pathName no longer found in cache."));
    }
  }
  for (SubscriptionVec::iterator it = m_subscribers.begin();
       it != m_subscribers.end(); ++it) {
    if (it->get() == sub) {
      m_subscribers.erase(it);
      MojLogInfo(s_log,
		 _T("CancelSubscription: Removed subscription on pathName '%s'."),
		 pathName.data());
      break;
    }
  }

  return MojErrNone;
}

MojErr
CategoryHandler::TouchCacheObject(MojServiceMessage* msg,
				  MojObject& payload) {

  MojLogTrace(s_log);

  MojString pathName;
  MojErr err = payload.getRequired(_T("pathName"), pathName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("TouchCacheObject: touching file '%s'."),
	      pathName.data());

  std::string msgText;
  const cachedObjectId_t objId = GetObjectIdFromPath(pathName.data());
  if (objId > 0) {
    if (GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
			    pathName.data()) ==
	m_fileCacheSet->GetTypeForObjectId(objId)) {
      if (m_fileCacheSet->Touch(objId)) {
	err = msg->replySuccess();
      } else {
	msgText = "TouchCacheObject: Could not locate object";
      }
    } else {
      msgText = "TouchCacheObject: pathName no longer found in cache.";
      MojLogError(s_log, _T("%s"), msgText.c_str());
    }
  } else {
      msgText = "TouchCacheObject: Invalid object id derived from pathname.";
      MojLogError(s_log, _T("%s"), msgText.c_str());
  }

  if (!msgText.empty()) {
    err = msg->replyError((MojErr) FCExistsError, msgText.c_str());
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::CopyCacheObject(MojServiceMessage* msg,
				 MojObject& payload) {

  MojLogTrace(s_log);

  MojString pathName, param;
  std::string destination, fileName;

  MojErr err = payload.getRequired(_T("pathName"), pathName);
  MojErrCheck(err);

  MojLogDebug(s_log, _T("CopyCacheObject: attempting to copy file '%s'."),
	      pathName.data());

  bool found = false;
  err = payload.get(_T("destination"), param, found);
  MojErrCheck(err);
  if (found && !param.empty()) {
    destination = param.data();
  } else {
    destination = s_defaultDownloadDir;
  }

  err = payload.get(_T("fileName"), param, found);
  MojErrCheck(err);

  std::string msgText;
  MojErr errCode = MojErrNone;
  const cachedObjectId_t objId = GetObjectIdFromPath(pathName.data());
  if (objId > 0) {
    if (GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
			    pathName.data()) ==
	m_fileCacheSet->GetTypeForObjectId(objId)) {
      if (m_fileCacheSet->CachedObjectSize(objId) < 0) {
	msgText = "CopyCacheObject: Could not locate object";
	errCode = (MojErr) FCExistsError;
	MojLogError(s_log, _T("%s"), msgText.c_str());
      } else {
	if (found && !param.empty()) {
	  fileName = param.data();
	} else {
	  fileName = m_fileCacheSet->CachedObjectFilename(objId);
	  if (fileName.empty()) {
	    msgText = "CopyCacheObject: No fileName specified or found.";
	    errCode = (MojErr) FCArgumentError;
	    MojLogError(s_log, _T("%s"), msgText.c_str());
	  }
	}
      }
    } else {
      msgText = "CopyCacheObject: pathName no longer found in cache.";
      errCode = (MojErr) FCExistsError;
      MojLogError(s_log, _T("%s"), msgText.c_str());
    }
  } else {
    msgText = "CopyCacheObject: Invalid object id derived from pathname.";
    errCode = (MojErr) FCExistsError;
    MojLogError(s_log, _T("%s"), msgText.c_str());
  }

  std::string destFileName;
  if (!SBIsPathAllowed(destination.c_str(), msg->senderName(), SB_WRITE | SB_CREATE)) {
    msgText = "CopyCacheObject: Invalid destination, no write permission.";
    errCode = (MojErr) FCPermError;
    MojLogError(s_log, _T("%s"), msgText.c_str());
  } else {
    try {
      fs::path filepath(destination);
      if (!fs::exists(filepath)) {
	fs::create_directories(filepath);
      }
      if (fs::is_directory(filepath)){
	int i = 1;
	std::string extension(GetFileExtension(fileName.c_str()));
	std::string basename(GetFileBasename(fileName.c_str()));
	while (fs::exists(filepath / fileName) &&
	       (i < s_maxUniqueFileIndex)) {
	  std::stringstream newFileName;
	  newFileName << basename << "-(" << i++ << ")" << extension;
	  fileName = newFileName.str();
	}
	if (i == s_maxUniqueFileIndex) {
	  msgText = "CopyCacheObject: No unique destination name found.";
	  errCode = (MojErr) FCArgumentError;
	  MojLogError(s_log, _T("%s"), msgText.c_str());
	} else {
	  destFileName = filepath.string() + "/" + fileName;
	}
      } else {
	msgText = "CopyCacheObject: Invalid destination, not a directory.";
	errCode = (MojErr) FCArgumentError;
	MojLogError(s_log, _T("%s"), msgText.c_str());
      }
    }
    catch (const fs::filesystem_error& ex) {
      if (ex.code().value() != 0) {
	msgText = "CopyCacheObject: ";
	msgText += ex.what();
	msgText += " (" + ex.code().message() + ").";
	MojLogError(s_log, _T("%s"), msgText.c_str());
	errCode = (MojErr) FCDirectoryError;
      }
    }
  }

  if (!msgText.empty()) {
    err = msg->replyError(errCode, msgText.c_str());
  } else {
    err = CopyFile(msg, pathName.data(), destFileName);
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::GetCacheStatus(MojServiceMessage* msg,
				MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  cacheSize_t numTypes = 0;
  cacheSize_t size = 0;
  cacheSize_t space = 0;
  paramValue_t numObjs = 0;

  numTypes = m_fileCacheSet->GetCacheStatus(&size, &numObjs, &space);

  MojObject reply;
  err = reply.putInt(_T("numTypes"), (MojInt64) numTypes);
  MojErrCheck(err);
  err = reply.putInt(_T("size"), (MojInt64) size);
  MojErrCheck(err);
  err = reply.putInt(_T("numObjs"), (MojInt64) numObjs);
  MojErrCheck(err);
  err = reply.putInt(_T("availSpace"), (MojInt64) space);
  MojErrCheck(err);
  MojLogDebug(s_log,
	      _T("GetCacheStatus: numTypes = '%d', size = '%d', numObjs = '%d', availSpace = '%d'."),
	      numTypes, size, numObjs, space);

  err = msg->replySuccess(reply);
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::GetCacheTypeStatus(MojServiceMessage* msg,
				    MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  cacheSize_t size = 0;
  paramValue_t numObjs = 0;

  MojString typeName;

  err = payload.getRequired(_T("typeName"), typeName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("GetCacheTypeStatus: getting status for type '%s'."),
	      typeName.data());
  bool suceeded =
    m_fileCacheSet->GetCacheTypeStatus(std::string(typeName.data()),
				       &size, &numObjs);
  MojObject reply;
  if (suceeded) {
    err = reply.putInt(_T("size"), (MojInt64) size);
    MojErrCheck(err);
    err = reply.putInt(_T("numObjs"), (MojInt64) numObjs);
    MojErrCheck(err);
    MojLogDebug(s_log, _T("GetCacheTypeStatus: size = '%d', numObjs = '%d'."),
		size, numObjs);
    err = msg->replySuccess(reply);
  } else {
    std::string msgText("GetCacheTypeStatus: Type '");
    msgText += typeName.data();
    msgText += "' doesn't exist";
    MojLogInfo(s_log, _T("%s"), msgText.c_str());
    err = msg->replyError((MojErr) FCExistsError, msgText.c_str());
  }

  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::GetCacheObjectSize(MojServiceMessage* msg,
				    MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString pathName;

  err = payload.getRequired(_T("pathName"), pathName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("GetCacheObjectSize: getting size for '%s'."),
	      pathName.data());

  MojObject reply;
  const cachedObjectId_t objId = GetObjectIdFromPath(pathName.data());
  cacheSize_t objSize = 0;
  if ((objId > 0) && ((objSize = m_fileCacheSet->CachedObjectSize(objId)) >= 0)) {
    err = reply.putInt(_T("size"), (MojInt64) objSize);
    MojErrCheck(err);
    MojLogDebug(s_log, _T("GetCacheObjectSize: found size '%d'."), objSize);
    err = msg->replySuccess(reply);
  } else {
    std::string msgText("GetCacheObjectSize: Object '");
    msgText += pathName.data();
    msgText += "' doesn't exist";
    MojLogInfo(s_log, _T("%s"), msgText.c_str());
    err = msg->replyError((MojErr) FCExistsError, msgText.c_str());
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::GetCacheObjectFilename(MojServiceMessage* msg,
					MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojString pathName;

  err = payload.getRequired(_T("pathName"), pathName);
  MojErrCheck(err);
  MojLogDebug(s_log, _T("GetCacheObjectFilename: getting filename for '%s'."),
	      pathName.data());

  MojObject reply;
  const cachedObjectId_t objId = GetObjectIdFromPath(pathName.data());
  if (objId > 0) {
    std::string filename = m_fileCacheSet->CachedObjectFilename(objId);
    err = reply.putString(_T("fileName"), filename.c_str());
    MojErrCheck(err);
    MojLogDebug(s_log, _T("GetCacheObjectFilename: found filename '%s'."),
		filename.c_str());
    err = msg->replySuccess(reply);
  } else {
    std::string msgText("GetCacheObjectFilename: Object '");
    msgText += pathName.data();
    msgText += "' doesn't exist";
    MojLogInfo(s_log, _T("%s"), msgText.c_str());
    err = msg->replyError((MojErr) FCExistsError, msgText.c_str());
  }
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::GetCacheTypes(MojServiceMessage* msg,
			       MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;

  MojObject reply;

  const std::vector<std::string> cacheTypes = m_fileCacheSet->GetTypes();
  if (!cacheTypes.empty()) {
    MojObject typeArray;
    std::vector<std::string>::const_iterator iter = cacheTypes.begin();
    while(iter != cacheTypes.end()) {
      err = typeArray.pushString((*iter).c_str());
      MojErrCheck(err);
      ++iter;
    }
    err = reply.put(_T("types"), typeArray);
    MojErrCheck(err);
    MojLogDebug(s_log, _T("GetCacheTypes: found '%zd' types."),
		cacheTypes.size());
  }
  err = msg->replySuccess(reply);
  MojErrCheck(err);

  return MojErrNone;
}

MojErr
CategoryHandler::GetVersion(MojServiceMessage* msg,
			    MojObject& payload) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;
  MojObject reply;

  err = reply.putString(_T("version"), s_InterfaceVersion.c_str());
  MojErrCheck(err);
  err = msg->replySuccess(reply);
  MojErrCheck(err);

  return MojErrNone;

}

MojErr
CategoryHandler::WorkerHandler() {

  MojLogTrace(s_log);

  MojLogDebug(s_log, _T("WorkerHandler: Attempting to cleanup any orphans."));
  m_fileCacheSet->CleanupOrphans();

  // For each subscribed object, if it's still being written, do a
  // validity check
  for (SubscriptionVec::const_iterator it = m_subscribers.begin();
       it != m_subscribers.end(); it++) {
    MojLogDebug(s_log, _T("WorkerHandler: Validating subscribed object '%s'."),
		(*it)->GetPathName().data());
    const cachedObjectId_t objId =
      GetObjectIdFromPath((*it)->GetPathName().data());
    const std::string typeName(GetTypeNameFromPath(m_fileCacheSet->GetBaseDirName(),
						   (*it)->GetPathName().data()));
    m_fileCacheSet->CheckSubscribedObject(typeName, objId);
  }

  return MojErrNone;
}

MojErr
CategoryHandler::CleanerHandler() {

  MojLogTrace(s_log);

  MojLogDebug(s_log, _T("CleanerHandler: Attempting to cleanup dirTypes."));
  m_fileCacheSet->CleanupDirTypes();

  return MojErrNone;
}

MojErr
CategoryHandler::SetupWorkerTimer() {

  MojLogTrace(s_log);

  g_timeout_add_seconds(15, &TimerCallback, this);
  g_timeout_add_seconds(120, &CleanerCallback, this);

  return MojErrNone;
}

gboolean
CategoryHandler::TimerCallback(void* data) {

  MojLogTrace(s_log);

  CategoryHandler* self = (CategoryHandler*) data;
  self->WorkerHandler();

  return true;
}

gboolean
CategoryHandler::CleanerCallback(void* data) {

  MojLogTrace(s_log);

  CategoryHandler* self = (CategoryHandler*) data;
  self->CleanerHandler();

  // return false here as this is a one shot
  return false;
}

CategoryHandler::Subscription::Subscription(CategoryHandler& handler,
					    MojServiceMessage* msg,
					    MojString& pathName)
  : m_handler(handler),
    m_msg(msg),
    m_pathName(pathName),
    m_cancelSlot(this, &Subscription::HandleCancel) {

  MojLogTrace(s_log);

  msg->notifyCancel(m_cancelSlot);
}

CategoryHandler::Subscription::~Subscription() {

  MojLogTrace(s_log);
}

MojErr
CategoryHandler::Subscription::HandleCancel(MojServiceMessage* msg) {

  MojLogTrace(s_log);

  return m_handler.CancelSubscription(this, msg, m_pathName);
}

MojErr
CategoryHandler::CopyFile(MojServiceMessage* msg,
			  const std::string& source,
			  const std::string& destination) {

  MojLogTrace(s_log);

  MojErr err = MojErrNone;

  CAsyncCopier* c = new CAsyncCopier(source, destination, msg);
  c->StartCopy();

  return err;
}

std::string
CategoryHandler::CallerID(MojServiceMessage* msg) {

  MojLogTrace(s_log);

  std::string caller;
  MojLunaMessage *lunaMsg = dynamic_cast<MojLunaMessage *>(msg);
  if (lunaMsg) {
    const char *appId = lunaMsg->appId();
    if (appId) {
      caller = appId;
      size_t firstSpace = caller.find_first_of(' ');
      if (firstSpace != std::string::npos) {
	caller.resize(firstSpace);
      }
    } else {
      const char *serviceId = lunaMsg->senderId();
      if (serviceId) {
	caller = serviceId;
      }
    }
  }

  return caller;
}
