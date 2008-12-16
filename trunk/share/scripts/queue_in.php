#!/usr/bin/php -q
<?
/*
 * Queue inbound calls (waiting in queue)
 * The queue module should let the call.execute fall through to this script.
 * It will optionally play a short greeting and then attach an on-hold source.
 *
 * To use add in queues.conf:
 * [channels]
 * incoming=external/nodata/queue_in.php
 */
require_once("libyate.php");

$ourcallid = "q-in/" . uniqid(rand(),1);
$partycallid = "";
$newsource = "";
$answermode = "";
$override = "";

function SendMsg($msg)
{
    global $ourcallid;
    global $partycallid;
    $m = new Yate($msg);
    $m->id = "";
    $m->params["id"] = $ourcallid;
    $m->params["peerid"] = $partycallid;
    $m->params["targetid"] = $partycallid;
    $m->params["reason"] = "queued";
    $m->Dispatch();
}

/* Always the first action to do */
Yate::Init();

/* Uncomment next line to get debugging messages */
//Yate::Debug(true);

Yate::SetLocal("id",$ourcallid);

/* The main loop. We pick events and handle them */
for (;;) {
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
	    switch ($ev->name) {
		case "call.execute":
		    $partycallid = $ev->params["id"];
		    if (array_key_exists("targetid",$ev->params))
			$ourcallid = $ev->params["targetid"];
		    else
			$ev->params["targetid"] = $ourcallid;
		    $answermode = $ev->GetValue("answermode","late");
		    $ev->handled = true;
		    /* We must ACK this message before dispatching a call.answered */
		    $ev->Acknowledge();

		    if ($ev->GetValue("source") || $ev->GetValue("consumer") || $ev->GetValue("greeting")) {
			$m = new Yate("chan.attach");
			$m->params["id"] = $ourcallid;
			if ($ev->GetValue("consumer"))
			    $m->params["consumer"] = $ev->GetValue("consumer");
			$newsource = $ev->GetValue("source");
			if ($ev->GetValue("greeting"))
			    $m->params["source"] = $ev->GetValue("greeting");
			else if ($newsource) {
			    $m->params["source"] = $newsource;
			    $newsource = "";
			}
			if ($newsource) {
			    $m->params["notify"] = $ourcallid;
			    Yate::Install("chan.notify",100,"targetid",$ourcallid);
			}
			else if ($answermode == "late")
			    $answermode = "early";
			$m->Dispatch();
		    }

		    switch ($answermode) {
			case "early":
			    SendMsg("call.answered");
			    break;
			case "late":
			case "never":
			    SendMsg("call.ringing");
			    break;
		    }

		    /* Prevent a warning if trying to ACK this message again */
		    $ev = false;
		    break;
		case "chan.notify":
		    Yate::Uninstall("chan.notify");
		    $m = new Yate("chan.attach");
		    $m->params["id"] = $ourcallid;
		    $m->params["source"] = $newsource;
		    $m->Dispatch();
		    $newsource = "";
		    if ($answermode == "late")
			SendMsg("call.answered");
		    break;
	    }
	    /* This is extremely important.
	       We MUST let messages return, handled or not */
	    if ($ev)
		$ev->Acknowledge();
	    break;
	default:
	    Yate::Debug("PHP Event: " . $ev->type);
    }
}

Yate::Debug("PHP: bye!");

/* vi: set ts=8 sw=4 sts=4 noet: */
?>
