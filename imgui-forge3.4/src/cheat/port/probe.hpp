#pragma once

namespace nl_cheat {

	// Dumps convar names and schema field offsets when %TEMP%\vesta\probe.on
	// exists, then removes the marker. Safe to call periodically.
	void run_probe_if_armed( );

} // namespace nl_cheat
