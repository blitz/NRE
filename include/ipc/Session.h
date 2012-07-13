/*
 * Copyright (C) 2012, Nils Asmussen <nils@os.inf.tu-dresden.de>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of NRE (NOVA runtime environment).
 *
 * NRE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NRE is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

#pragma once

#include <utcb/UtcbFrame.h>
#include <ipc/Connection.h>
#include <ipc/Service.h>
#include <cap/CapSpace.h>
#include <kobj/Pt.h>
#include <CPU.h>

namespace nre {

/**
 * Represents a session at a service. This way the service can manage per-session-data. That is,
 * it can distinguish between clients.
 */
class Session {
public:
	enum Command {
		OPEN,
		SHARE_DATASPACE,
		CLOSE
	};

	/**
	 * Opens the session at the service specified by the given connection
	 *
	 * @param con the connection to the service
	 * @throws Exception if the session-creation failed
	 */
	explicit Session(Connection &con) : _caps(open(con)), _con(con) {
	}
	/**
	 * Closes the session again
	 */
	virtual ~Session() {
		close();
		CapSpace::get().free(_caps,CPU::count());
	}

	/**
	 * @return the connection
	 */
	Connection &con() {
		return _con;
	}
	/**
	 * @return the base of the capabilities received to communicate with the service
	 */
	capsel_t caps() const {
		return _caps;
	}

private:
	capsel_t open(Connection &con) {
		UtcbFrame uf;
		ScopedCapSels caps(CPU::count(),CPU::count());
		uf.delegation_window(Crd(caps.get(),Math::next_pow2_shift<uint>(CPU::count()),Crd::OBJ_ALL));
		uf << OPEN;
		con.pt(CPU::current().log_id())->call(uf);

		ErrorCode res;
		uf >> res;
		if(res != E_SUCCESS)
			throw Exception(res);
		return caps.release();
	}

	void close() {
		UtcbFrame uf;
		uf.translate(_caps + CPU::current().log_id());
		uf << CLOSE;
		_con.pt(CPU::current().log_id())->call(uf);
	}

	Session(const Session&);
	Session& operator=(const Session&);

	capsel_t _caps;
	Connection &_con;
};

}
