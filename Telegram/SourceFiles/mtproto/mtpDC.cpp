/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014 John Preston, https://desktop.telegram.org
*/
#include "stdafx.h"
#include "mtpDC.h"
#include "mtp.h"

#include "localstorage.h"

namespace {
	
	MTProtoDCMap gDCs;
	bool configLoadedOnce = false;
	bool mainDCChanged = false;
	int32 mainDC = 2;
	int32 userId = 0;

	typedef QMap<int32, mtpAuthKeyPtr> _KeysMapForWrite;
	_KeysMapForWrite _keysMapForWrite;
	QMutex _keysMapForWriteMutex;
}

int32 mtpAuthed() {
	return userId;
}

void mtpAuthed(int32 uid) {
	if (userId != uid) {
		userId = uid;
	}
}

MTProtoDCMap &mtpDCMap() {
	return gDCs;
}

bool mtpNeedConfig() {
	return !configLoadedOnce;
}

int32 mtpMainDC() {
	return mainDC;
}

void mtpLogoutOtherDCs() {
	QList<int32> dcs;
	{
		QMutexLocker lock(&_keysMapForWriteMutex);
		dcs = _keysMapForWrite.keys();
	}
	for (int32 i = 0, cnt = dcs.size(); i != cnt; ++i) {
		if (dcs[i] != MTP::maindc()) {
			MTP::send(MTPauth_LogOut(), RPCResponseHandler(), dcs[i]);
		}
	}
}

void mtpSetDC(int32 dc, bool firstOnly) {
	if (!dc || (firstOnly && mainDCChanged)) return;
	mainDCChanged = true;
	if (dc != mainDC) {
		mainDC = dc;
	}
}

MTProtoDC::MTProtoDC(int32 id, const mtpAuthKeyPtr &key) : _id(id), _key(key), _connectionInited(false) {
	connect(this, SIGNAL(authKeyCreated()), this, SLOT(authKeyWrite()), Qt::QueuedConnection);

	QMutexLocker lock(&_keysMapForWriteMutex);
	if (_key) {
		_keysMapForWrite[_id] = _key;
	} else {
		_keysMapForWrite.remove(_id);
	}
}

void MTProtoDC::authKeyWrite() {
	DEBUG_LOG(("AuthKey Info: MTProtoDC::authKeyWrite() slot, dc %1").arg(_id));
	if (_key) {
		Local::writeMtpData();
	}
}

void MTProtoDC::setKey(const mtpAuthKeyPtr &key) {
	DEBUG_LOG(("AuthKey Info: MTProtoDC::setKey(%1), emitting authKeyCreated, dc %2").arg(key ? key->keyId() : 0).arg(_id));
	_key = key;
	_connectionInited = false;
	emit authKeyCreated();

	QMutexLocker lock(&_keysMapForWriteMutex);
	if (_key) {
		_keysMapForWrite[_id] = _key;
	} else {
		_keysMapForWrite.remove(_id);
	}
}

QReadWriteLock *MTProtoDC::keyMutex() const {
	return &keyLock;
}

const mtpAuthKeyPtr &MTProtoDC::getKey() const {
	return _key;
}

void MTProtoDC::destroyKey() {
	setKey(mtpAuthKeyPtr());

	QMutexLocker lock(&_keysMapForWriteMutex);
	_keysMapForWrite.remove(_id);
}

namespace {
	MTProtoConfigLoader *configLoader = 0;
	bool loadingConfig = false;
	void configLoaded(const MTPConfig &result) {
		loadingConfig = false;

		const MTPDconfig &data(result.c_config());

		DEBUG_LOG(("MTP Info: got config, chat_size_max: %1, date: %2, test_mode: %3, this_dc: %4, dc_options.length: %5").arg(data.vchat_size_max.v).arg(data.vdate.v).arg(data.vtest_mode.v).arg(data.vthis_dc.v).arg(data.vdc_options.c_vector().v.size()));

		mtpUpdateDcOptions(data.vdc_options.c_vector().v);
		cSetMaxGroupCount(data.vchat_size_max.v);

		configLoadedOnce = true;
		Local::writeSettings();

		mtpConfigLoader()->done();
	}
	bool configFailed(const RPCError &err) {
		loadingConfig = false;
		LOG(("MTP Error: failed to get config!"));
		return false;
	}
};

void mtpUpdateDcOptions(const QVector<MTPDcOption> &options) {
	QSet<int32> already, restart;
	{
		mtpDcOptions opts(cDcOptions());
		for (QVector<MTPDcOption>::const_iterator i = options.cbegin(), e = options.cend(); i != e; ++i) {
			const MTPDdcOption &optData(i->c_dcOption());
			if (already.constFind(optData.vid.v) == already.cend()) {
				already.insert(optData.vid.v);
				mtpDcOptions::const_iterator a = opts.constFind(optData.vid.v);
				if (a != opts.cend()) {
					if (a.value().ip != optData.vip_address.c_string().v || a.value().port != optData.vport.v) {
						restart.insert(optData.vid.v);
					}
				}
				opts.insert(optData.vid.v, mtpDcOption(optData.vid.v, optData.vhostname.c_string().v, optData.vip_address.c_string().v, optData.vport.v));
			}
		}
		cSetDcOptions(opts);
	}
	for (QSet<int32>::const_iterator i = restart.cbegin(), e = restart.cend(); i != e; ++i) {
		MTP::restart(*i);
	}
}

MTProtoConfigLoader::MTProtoConfigLoader() : _enumCurrent(0), _enumRequest(0) {
	connect(&_enumDCTimer, SIGNAL(timeout()), this, SLOT(enumDC()));
	connect(this, SIGNAL(killCurrentSession(qint32,qint32)), this, SLOT(onKillCurrentSession(qint32,qint32)), Qt::QueuedConnection);
}

void MTProtoConfigLoader::load() {
	if (loadingConfig) return;
	loadingConfig = true;

	MTP::send(MTPhelp_GetConfig(), rpcDone(configLoaded), rpcFail(configFailed));

	_enumDCTimer.start(MTPEnumDCTimeout);
}

void MTProtoConfigLoader::onKillCurrentSession(qint32 request, qint32 current) {
	if (request == _enumRequest && current == _enumCurrent) {
		if (_enumRequest) {
			MTP::cancel(_enumRequest);
			_enumRequest = 0;
		}
		if (_enumCurrent) {
			MTP::killSession(MTP::cfg + _enumCurrent);
			_enumCurrent = 0;
		}
	}
}

void MTProtoConfigLoader::done() {
	_enumDCTimer.stop();
	if (_enumRequest || _enumCurrent) {
		emit killCurrentSession(_enumRequest, _enumCurrent);
	}
	emit loaded();
}

void MTProtoConfigLoader::enumDC() {
	if (!loadingConfig) return;

	if (_enumRequest) MTP::cancel(_enumRequest);

	if (!_enumCurrent) {
		_enumCurrent = mainDC;
	} else {
		MTP::killSession(MTP::cfg + _enumCurrent);
	}
	const mtpDcOptions &options(cDcOptions());
	for (mtpDcOptions::const_iterator i = options.cbegin(), e = options.cend(); i != e; ++i) {
		if (i.key() == _enumCurrent) {
			_enumCurrent = (++i == e) ? options.cbegin().key() : i.key();
			break;
		}
	}

	_enumRequest = MTP::send(MTPhelp_GetConfig(), rpcDone(configLoaded), rpcFail(configFailed), MTP::cfg + _enumCurrent);

	_enumDCTimer.start(MTPEnumDCTimeout);
}

MTProtoConfigLoader *mtpConfigLoader() {
	if (!configLoader) configLoader = new MTProtoConfigLoader();
	return configLoader;
}

void mtpDestroyConfigLoader() {
	delete configLoader;
	configLoader = 0;
}

mtpKeysMap mtpGetKeys() {
	mtpKeysMap result;
	QMutexLocker lock(&_keysMapForWriteMutex);
	for (_KeysMapForWrite::const_iterator i = _keysMapForWrite.cbegin(), e = _keysMapForWrite.cend(); i != e; ++i) {
		result.push_back(i.value());
	}
	return result;
}

void mtpSetKey(int32 dcId, mtpAuthKeyPtr key) {
	MTProtoDCPtr dc(new MTProtoDC(dcId, key));
	gDCs.insert(dcId, dc);
}
