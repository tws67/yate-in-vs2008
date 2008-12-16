#!/usr/bin/php -q
<?
/* Sample script for the Yate PHP interface.
   Implements SIP SUBSCRIBE and NOTIFY for voicemail and dialog state.

   To test add in extmodule.conf

   [scripts]
   sipfeatures.php=
*/
require_once("libyate.php");
require_once("libvoicemail.php");

$next = 0;

// list of user voicemail subscriptions
$users = array();
// list of active channels (dialog) subscriptions
$chans = array();

// Abstract subscription object
class Subscription {
    var $match;
    var $index;
    var $event;
    var $media;
    var $host;
    var $port;
    var $uri;
    var $from;
    var $to;
    var $callid;
    var $contact;
    var $state;
    var $body = "";
    var $expire = 0;
    var $pending = false;

    // Constructor, fills internal variables and sends initial/final notifications
    function Subscription($ev,$event,$media)
    {
	$this->event = $event;
	$this->media = $media;
	$this->host = $ev->params["ip_host"];
	$this->port = $ev->params["ip_port"];
	$this->uri = $ev->params["sip_uri"];
	// allow the mailbox to be referred as vm-NUMBER (or anything-NUMBER)
	ereg(":([a-z]*-)?([^:@]+)@",$this->uri,$regs);
	$this->match = $regs[2];
	$this->index = $event .":". $this->match .":". $this->host .":". $this->port;
	Yate::Debug("Will match: " . $this->match . " index " . $this->index);
	$this->from = $ev->GetValue("sip_from");
	$this->to = $ev->GetValue("sip_to");
	if (strpos($this->to,'tag=') === false)
	    $this->to .= ';tag=' . $ev->GetValue("xsip_dlgtag");
	$this->callid = $ev->params["sip_callid"];
	$this->contact = $ev->params["sip_contact"];
	$exp = $ev->params["sip_expires"];
	if ($exp == "0") {
	    // this is an unsubscribe
	    $this->expire = 0;
	    $this->state = "terminated";
	}
	else {
	    $exp = $exp + 0;
	    if ($exp <= 0)
		$exp = 3600;
	    else if ($exp < 60)
		$exp = 60;
	    else if ($exp > 86400)
		$exp = 86400;
	    $this->expire = time() + $exp;
	    $this->state = "active";
	}
	$this->body = "";
	$this->pending = true;
    }

    // Send out a NOTIFY
    function Notify($state = false)
    {
	Yate::Debug("Notifying event " . $this->event . " for " . $this->match);
	if ($state !== false) {
	    $this->state = $state;
	    $this->pending = true;
	}
	if ($this->body == "") {
	    Yate::Output("Empty body in event " . $this->event . " for " . $this->match);
	    return;
	}
	$this->pending = false;
	$m = new Yate("xsip.generate");
	$m->id = "";
	$m->params["method"] = "NOTIFY";
	$m->params["uri"] = $this->contact;
	$m->params["host"] = $this->host;
	$m->params["port"] = $this->port;
	$m->params["sip_Call-ID"] = $this->callid;
	$m->params["sip_From"] = $this->to;
	$m->params["sip_To"] = $this->from;
	$m->params["sip_Contact"] = "<" . $this->uri . ">";
	$m->params["sip_Event"] = $this->event;
	$m->params["sip_Subscription-State"] = $this->state;
	$m->params["xsip_type"] = $this->media;
	$m->params["xsip_body"] = $this->body;
	$m->Dispatch();
    }

    // Add this object to the parent list
    function AddTo(&$list)
    {
	$list[$this->index] = &$this;
    }

    // Emit any pending NOTIFY
    function Flush()
    {
	if ($this->pending)
	    $this->Notify();
    }

    // Check expiration of this subscription, prepares timeout notification
    function Expire()
    {
	if ($this->expire == 0)
	    return false;
	if ($this->expire >= time())
	    return false;
	Yate::Debug("Expired event " . $this->event . " for " . $this->match);
	$this->expire = 0;
	$this->Notify("terminated;reason=timeout");
	return true;
    }

}

// Mailbox status subscription
class MailSub extends Subscription {

    function MailSub($ev) {
	global $users;
	parent::Subscription($ev,"message-summary","application/simple-message-summary");
    }

    // Update count of messages, calls voicemail library function
    function Update($ev,$id)
    {
	vmGetMessageStats($this->match,$tot,$unr);
	Yate::Debug("Messages: $unr/$tot for " . $this->match);
	if ($tot > 0) {
	    $mwi = ($unr > 0) ? "yes" : "no";
	    $this->body = "Messages-Waiting: $mwi\r\nVoice-Message: $unr/$tot\r\n";
	}
	else
	    $this->body = "Messages-Waiting: no\r\n";
	$this->pending = true;
    }

}

// SIP dialog status subscription
class DialogSub extends Subscription {

    var $version;

    function DialogSub($ev) {
	global $chans;
	parent::Subscription($ev,"dialog","application/dialog-info+xml");
	$this->version = 0;
    }

    // Update dialog state based on CDR message parameters
    function Update($ev,$id)
    {
	$st = "";
	$dir = "";
	switch ($ev->GetValue("operation")) {
	    case "initialize":
		$st = "trying";
		break;
	    case "finalize":
		$st = "terminated";
		break;
	    default:
		switch ($ev->GetValue("status")) {
		    case "connected":
		    case "answered":
			$st = "confirmed";
			break;
		    case "incoming":
		    case "outgoing":
		    case "calling":
		    case "ringing":
		    case "progressing":
			$st = "early";
			break;
		    case "redirected":
			$st = "rejected";
			break;
		    case "destroyed":
			$st = "terminated";
			break;
		}
	}
	if ($st != "") {
	    // directions are reversed because we are talking about the remote end
	    switch ($ev->GetValue("direction")) {
		case "incoming":
		    $dir = "initiator";
		    break;
		case "outgoing":
		    $dir = "recipient";
		    break;
	    }
	    if ($dir != "")
		$dir = " direction=\"$dir\"";
	}
	else
	    $id = false;
	Yate::Debug("Dialog updated, st: '$st' id: '$id'");
	$this->body = "<?xml version=\"1.0\"?>\r\n";
	$this->body .= "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"" . $this->version . "\" entity=\"" . $this->uri . "\" notify-state=\"full\">\r\n";
	if ($id) {
	    $uri = ereg_replace("^.*<([^<>]+)>.*\$","\\1",$this->uri);
	    $tag = $this->match;
	    $tag = " call-id=\"$id\" local-tag=\"$tag\" remote-tag=\"$tag\"";
	    $this->body .= "  <dialog id=\"$id\"$tag$dir>\r\n";
	    $this->body .= "    <state>$st</state>\r\n";
	    $this->body .= "    <remote><target uri=\"$uri\"/></remote>\r\n";
	    $this->body .= "  </dialog>\r\n";
	}
	$this->body .= "</dialog-info>\r\n";
	$this->version++;
	$this->pending = true;
	if ($id)
	    $this->Flush();
    }
}

// Update all subscriptions in a $list that match a given $key
function updateAll(&$list,$key,$ev,$id = false)
{
    if (!$key)
	return;
    $count = 0;
    foreach ($list as &$item) {
	if ($item->match == $key) {
	    $item->Update($ev,$id);
	    $count++;
	}
    }
    Yate::Debug("Updated $count subscriptions for '$key'");
}

// Flush all pending notifies for subscriptions in a $list
function flushAll(&$list)
{
    foreach ($list as &$item)
	$item->Flush();
}

// Check all subscriptions in $list for expiration, notifies and removes them
function expireAll(&$list)
{
    foreach ($list as $index => &$item) {
	if ($item->Expire()) {
	    $list[$index] = null;
	    unset($list[$index]);
	}
    }
}

// List subscriptions in $list to a $text variable
function dumpAll(&$list,&$text)
{
    foreach ($list as &$item) {
	$e = $item->expire;
	if ($e > 0)
	    $e -= time();
	$text .= $item->index . " expires in $e\r\n";
    }
}

// SIP SUBSCRIBE handler
function onSubscribe($ev)
{
    global $users;
    global $chans;
    $event = $ev->GetValue("sip_event");
    $accept = $ev->GetValue("sip_accept");
    if (($event == "message-summary") && ($accept == "application/simple-message-summary")) {
	$s = new MailSub($ev);
	$s->AddTo($users);
	Yate::Debug("New mail subscription for " . $s->match);
    }
    else if (($event == "dialog") && ($accept == "application/dialog-info+xml")) {
	$s = new DialogSub($ev);
	$s->AddTo($chans);
	Yate::Debug("New dialog subscription for " . $s->match);
    }
    else
	return false;
    $s->Update($ev,false);
    $s->Flush();
    return true;
}

// User status (mailbox) update handler
function onUserUpdate($ev)
{
    global $users;
    updateAll($users,$ev->GetValue("user"),$ev);
    return false;
}

// Channel status (dialog) handler, not currently used
function onChanUpdate($ev)
{
    global $chans;
    updateAll($chans,$ev->GetValue("id"),$ev);
    return false;
}

// CDR handler for channel status update
function onCdr($ev)
{
    global $chans;
    updateAll($chans,$ev->GetValue("external"),$ev,$ev->GetValue("chan"));
    return false;
}

// Timer message handler, expires and flushes subscriptions
function onTimer($t)
{
    global $users;
    global $chans;
    global $next;
    if ($t < $next)
	return;
    // Next check in 15 seconds
    $next = $t + 15;
    // Yate::Debug("Expiring aged subscriptions at $t");
    expireAll($users);
    expireAll($chans);
    // Yate::Debug("Flushing pending subscriptions at $t");
    flushAll($users);
    flushAll($chans);
}

// Command message handler
function onCommand($l,&$retval)
{
    global $users;
    global $chans;
    if ($l == "sippbx list") {
	$retval .= "Subscriptions:\r\n";
	dumpAll($users,$retval);
	dumpAll($chans,$retval);
	return true;
    }
    return false;
}

/* Always the first action to do */
Yate::Init();

Yate::Debug(true);

Yate::SetLocal("restart",true);

Yate::Install("sip.subscribe");
Yate::Install("user.update");
Yate::Install("chan.update");
Yate::Install("call.cdr");
Yate::Install("engine.timer");
Yate::Install("engine.command");

/* Create and dispatch an initial test message */
/* The main loop. We pick events and handle them */
for (;;) {
    $ev=Yate::GetEvent();
    /* If Yate disconnected us then exit cleanly */
    if ($ev === false)
        break;
    /* Empty events are normal in non-blocking operation.
       This is an opportunity to do idle tasks and check timers */
    if ($ev === true) {
        continue;
    }
    /* If we reached here we should have a valid object */
    switch ($ev->type) {
	case "incoming":
	    switch ($ev->name) {
		case "engine.timer":
		    onTimer($ev->GetValue("time"));
		    break;
		case "engine.command":
		    $ev->handled = onCommand($ev->GetValue("line"),$ev->retval);
		    break;
		case "sip.subscribe":
		    $ev->handled = onSubscribe($ev);
		    break;
		case "user.update":
		    $ev->handled = onUserUpdate($ev);
		    break;
		case "chan.update":
		    $ev->handled = onChanUpdate($ev);
		    break;
		case "call.cdr":
		    $ev->handled = onCdr($ev);
		    break;
	    }
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
	    Yate::Output("PHP Event: " . $ev->type);
    }
}

Yate::Output("PHP: bye!");

/* vi: set ts=8 sw=4 sts=4 noet: */
?>