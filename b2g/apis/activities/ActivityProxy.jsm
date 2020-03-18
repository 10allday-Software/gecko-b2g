/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

function debug(aMsg) {
  //dump("-- ActivityProxy: " + aMsg + "\n");
}

/*
 * ActivityProxy is a helper of passing requests from WebActivity.cpp to
 * ActivitiesService.jsm, receiving results from ActivitiesService and send it
 * back to WebActivity.
 */

function ActivityProxy() {}

ActivityProxy.prototype = {
  activities: new Map(),

  init(owner, options, url) {
    let id = Cc["@mozilla.org/uuid-generator;1"]
      .getService(Ci.nsIUUIDGenerator)
      .generateUUID()
      .toString();

    this.activities.set(id, {
      owner: owner ? owner : {},
      options,
      pageURL: url,
      childID: Services.appinfo.uniqueProcessID,
    });

    debug("Create Activity with id: " + id);

    // TODO: nsIPrincipal does not provide App related attributes such as appId
    // anymore, and there's no way to retrieve app's manifestURL now, so
    // security check here is left unimplemented. (Bug 80948)

    // TODO: The old implementation check for pref "dom.apps.developer_mode" and
    // "dom.activities.developer_mode_only" to see whether the activities are
    // restricted to be used in developer mode. Remove it since we are unsure
    // about porting back the app developer mode.

    // TODO: We used to use _getHashForApp to validate app identity, but there's
    // no way to retrieve app's manifestURL currently. (Bug 80948)

    return id;
  },

  start(id, callback) {
    let activity = this.activities.get(id);
    activity.callback = callback;

    Services.cpmm.addMessageListener("Activity:FireSuccess", this);
    Services.cpmm.addMessageListener("Activity:FireError", this);
    Services.cpmm.addMessageListener("Activity:FireCancel", this);
    Services.cpmm.sendAsyncMessage("Activity:Start", {
      id,
      options: activity.options,
      manifestURL: "", //TODO: Retrieve app's manifestURL.
      pageURL: activity.pageURL,
      childID: activity.childID,
    });
  },

  cancel(id) {
    Services.cpmm.sendAsyncMessage("Activity:Cancel", {
      id,
      error: "ACTIVITY_CANCELED",
    });
  },

  receiveMessage(message) {
    let msg = message.json;

    let activity = this.activities.get(msg.id);
    if (!activity) {
      return;
    }
    debug("Activity id: " + msg.id + ", handles message: " + message.name);

    switch (message.name) {
      case "Activity:FireSuccess":
        activity.callback.onStart(
          Cr.NS_OK,
          Cu.cloneInto(msg.result, activity.owner)
        );
        break;
      case "Activity:FireCancel":
      case "Activity:FireError":
        activity.callback.onStart(
          Cr.NS_ERROR_FAILURE,
          Cu.cloneInto(msg.error, activity.owner)
        );
        break;
    }
    Services.cpmm.removeMessageListener("Activity:FireSuccess", this);
    Services.cpmm.removeMessageListener("Activity:FireError", this);
    Services.cpmm.removeMessageListener("Activity:FireCancel", this);
    this.activities.delete(msg.id);
  },

  contractID: "@mozilla.org/dom/activities/proxy;1",

  classID: Components.ID("{1d069f6a-bb82-4648-8bcd-b8671b4a963d}"),

  QueryInterface: ChromeUtils.generateQI([Ci.nsIActivityProxy]),
};

//module initialization
var EXPORTED_SYMBOLS = ["ActivityProxy"];
