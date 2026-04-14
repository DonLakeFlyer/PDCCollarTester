on run
	-- Find the binary inside our own app bundle
	set myPath to POSIX path of (path to me)
	set macosFolder to myPath & "Contents/MacOS/"
	set tester to quoted form of (macosFolder & "collar_tester")

	-- Load last-used frequency or default
	set freqFile to macosFolder & "last_freq.txt"
	set defaultFreq to "146.000"
	try
		set defaultFreq to do shell script "cat " & quoted form of freqFile
	end try

	-- Get frequency
	set freqStr to text returned of (display dialog "Enter collar frequency in MHz:" default answer defaultFreq buttons {"Cancel", "OK"} default button "OK")

	-- Save frequency for next run
	do shell script "echo " & quoted form of freqStr & " > " & quoted form of freqFile

	-- Auto-calibrate if no reference file exists, otherwise test
	set refFile to macosFolder & "collar_ref.dat"
	tell application "System Events"
		set refExists to exists file refFile
	end tell

	if refExists then
		-- Check if reference is older than 24 hours
		set refAge to do shell script "echo $(( ($(date +%s) - $(stat -f %m " & quoted form of refFile & ")) / 3600 ))"
		set refAgeNum to refAge as integer
		if refAgeNum ≥ 24 then
			display dialog "Reference calibration is " & refAge & " hours old and has expired. Please re-calibrate with a known-good collar." buttons {"Cancel", "Calibrate"} default button "Calibrate"
			set mode to "Calibrate"
		else
			set mode to button returned of (display dialog "Reference file found (" & refAge & " hours old). Calibrate with a new reference collar, or test a collar?" buttons {"Cancel", "Test", "Calibrate"} default button "Test")
		end if
	else
		display dialog "No reference file found. Please calibrate with a known-good collar first." buttons {"Cancel", "Calibrate"} default button "Calibrate"
		set mode to "Calibrate"
	end if

	if mode is "Calibrate" then
		set cmd to tester & " --freq " & freqStr & " --calibrate"
	else
		set cmd to tester & " --freq " & freqStr
	end if

	-- Show a "please wait" dialog in a background process during capture
	set waitPid to do shell script "osascript -e 'display dialog \"Warming up and capturing data for ~15 seconds. Please wait...\" giving up after 60 buttons {\"Please Wait...\"} default button 1 with title \"CollarTest\"' &>/dev/null & echo $!"

	-- Run the binary
	try
		set output to do shell script cmd
		-- Kill the wait dialog
		do shell script "kill " & waitPid & " 2>/dev/null; true"
		-- Check for GOOD/BAD in output
		if output contains "*** GOOD ***" then
			display dialog output buttons {"OK"} default button "OK" with icon note with title "PASS"
		else if output contains "*** BAD ***" then
			display dialog output buttons {"OK"} default button "OK" with icon stop with title "FAIL"
		else
			display dialog output buttons {"OK"} default button "OK" with title "Result"
		end if
	on error errMsg
		-- Kill the wait dialog
		do shell script "kill " & waitPid & " 2>/dev/null; true"
		-- do shell script throws on non-zero exit code; stderr + stdout in errMsg
		if errMsg contains "*** BAD ***" then
			display dialog errMsg buttons {"OK"} default button "OK" with icon stop with title "FAIL"
		else
			display dialog "Error running test:" & return & return & errMsg buttons {"OK"} default button "OK" with icon stop with title "Error"
		end if
	end try
end run
