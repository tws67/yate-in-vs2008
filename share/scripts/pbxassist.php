#!/usr/bin/php -q
<?
/* Sample PBX assistant for the Yate PHP interface
To use add in regexroute.conf

   ^NNN$=external/nochan/pbxassist.php;real_callto=real/resource/to/call

You will also need a priority= in extmodule.conf [general] in range 50-85
*/
require_once("libyate.php");

$ourcallid = true;

function onStartup(&$ev)
{
    global $ourcallid;
    Yate::Output("Channel $ourcallid is being assisted");
}

function onHangup($ev)
{
    global $ourcallid;
    Yate::Output("Channel $ourcallid has hung up");
}

function onDisconnect(&$ev,$reason)
{
    global $ourcallid;
    Yate::Output("Channel $ourcallid was disconnected, reason: '$reason'");
    // Sample action: redirect to info tone if user is busy
    if ($reason == "busy") {
	$m = new Yate("call.execute");
	$m->id = $ev->id;
	$m->SetParam("id",$ourcallid);
	$m->SetParam("callto","tone/info");
	$m->Dispatch();
	// Also send progressing so the tone goes through in early media
	$m = new Yate("call.progress");
	$m->SetParam("targetid",$ourcallid);
	$m->Dispatch();
	return true;
    }
    return false;
}

/* Always the first action to do */
Yate::Init();

/* The main loop. We pick events and handle them */
while ($ourcallid) {
    $ev=Yate::GetEvent();
    /* If Yate disconnected us then exit cleanly */
    if ($ev === false)
	break;
    /* No need to handle empty events in this application */
    if ($ev === true)
	continue;
    /* If we reached here we should have a valid object */
    switch ($ev->type) {
	case "incoming":
	    // Yate::Debug("PHP Message: " . $ev->name . " id: " . $ev->id);
	    switch ($ev->name) {
		case "call.execute":
		    $ourcallid = $ev->GetValue("id");
		    $callto = $ev->GetValue("real_callto");
		    if ($ourcallid && $callto) {
			// Put back the real callto and let the message flow
			$ev->SetParam("callto",$callto);
			Yate::Install("chan.hangup",75,"id",$ourcallid);
			Yate::Install("chan.disconnected",75,"id",$ourcallid);
			onStartup($ev);
		    }
		    else {
			Yate::Output("Invalid assist: '$ourcallid' -> '$callto'");
			$ourcallid = false;
		    }
		    break;
		case "chan.hangup":
		    // We were hung up. Do any cleanup and exit.
		    onHangup($ev);
		    $ourcallid = false;
		    break;
		case "chan.disconnected":
		    // Our party disconnected and we're ready to hang up.
		    // We should reconnect before this message is acknowledged
		    if (onDisconnect($ev,$ev->GetValue("reason")))
			$ev->handled = true;
		    break;
	    }
	    /* This is extremely important.
	       We MUST let messages return, handled or not */
	    if ($ev !== false)
		$ev->Acknowledge();
	    break;
	case "answer":
	    // Yate::Debug("PHP Answered: " . $ev->name . " id: " . $ev->id);
	    break;
	case "installed":
	    // Yate::Debug("PHP Installed: " . $ev->name);
	    break;
	case "uninstalled":
	    // Yate::Debug("PHP Uninstalled: " . $ev->name);
	    break;
	default:
	    // Yate::Output("PHP Event: " . $ev->type);
    }
}

Yate::Output("PHP: bye!");

/* vi: set ts=8 sw=4 sts=4 noet: */
?>
