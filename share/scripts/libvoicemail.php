<?

/* libvoicemail.php
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Voicemail access functions
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

require_once("libyate.php");

$vm_base = "/var/spool/voicemail";
$vm_func_for_dir = "vmDefaultGetDir";

function vmGetMessageStats($mailbox,&$total,&$unread,$type = "voicemail")
{
    $o = 0;
    $n = 0;
    $dir = vmGetVoicemailDir($mailbox);
    if (is_dir($dir) && ($d = @opendir($dir))) {
	while (($f = readdir($d)) !== false) {
	    if (substr($f,0,4) == "nvm-") {
		Yate::Debug("found new file '$f'");
		$n++;
	    }
	    else if (substr($f,0,3) == "vm-") {
		Yate::Debug("found old file '$f'");
		$o++;
	    }
	}
	closedir($d);
    }
    $total = $n + $o;
    $unread = $n;
}

function vmGetMessageFiles($mailbox,&$files)
{
    $dir = vmGetVoicemailDir($mailbox);
    if (is_dir($dir) && ($d = @opendir($dir))) {
	$nf = array();
	$of = array();
	while (($f = readdir($d)) !== false) {
	    if (substr($f,0,4) == "nvm-") {
		Yate::Debug("found new file '$f'");
		$nf[] = $f;
	    }
	    else if (substr($f,0,3) == "vm-") {
		Yate::Debug("found old file '$f'");
		$of[] = $f;
	    }
	}
	closedir($d);
	$files = array_merge($nf,$of);
	return true;
    }
    return false;
}

function vmSetMessageRead($mailbox,&$file)
{
    $dir = vmGetVoicemailDir($mailbox);
    if (is_dir($dir) && is_file("$dir/$file")) {
	if (substr($file,0,4) != "nvm-")
	    return false;
	$newname = substr($file,1);
	if (rename("$dir/$file","$dir/$newname")) {
	    $file = $newname;
	    return true;
	}
    }
    return false;
}

function vmInitMessageDir($mailbox)
{
    $dir = vmGetVoicemailDir($mailbox);
    if (!is_dir($dir))
	mkdir($dir,0750);
}

function vmHasMessageDir($mailbox)
{
    return is_dir(vmGetVoicemailDir($mailbox));
}

function vmBuildNewFilename($caller)
{
    $tmp = strftime("%Y.%m.%d-%H.%M.%S");
    $tmp = "nvm-$tmp-$caller.slin";
    return $tmp;
}

function vmGetVoicemailDir($called)
{
	global $vm_func_for_dir;

	return call_user_func($vm_func_for_dir,$called);
}

function vmDefaultGetDir($called)
{
	global $vm_base;

	return "$vm_base/$called";
}

/* vi: set ts=8 sw=4 sts=4 noet: */
