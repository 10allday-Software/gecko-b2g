/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
const { ComponentUtils } = ChromeUtils.import(
  "resource://gre/modules/ComponentUtils.jsm"
);
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

function oopCommandlineHandler() {}

oopCommandlineHandler.prototype = {
  handle(cmdLine) {
    let oopFlag = cmdLine.handleFlag("oop", false);
    if (oopFlag) {
      /**
       * Manipulate preferences by adding to the *default* branch.  Adding
       * to the default branch means the changes we make won"t get written
       * back to user preferences.
       */
      let prefs = Services.prefs;
      let branch = prefs.getDefaultBranch("");

      try {
        // Turn on all OOP services, making desktop run similar to phone
        // environment
        branch.setBoolPref("dom.ipc.tabs.disabled", false);
        branch.setBoolPref("layers.acceleration.disabled", false);
        branch.setBoolPref(
          "layers.offmainthreadcomposition.async-animations",
          true
        );
        branch.setBoolPref("layers.async-video.enabled", true);
        branch.setBoolPref("layers.async-pan-zoom.enabled", true);
        branch.setCharPref("gfx.content.azure.backends", "cairo");
      } catch (e) {}
    }
    if (cmdLine.state == Ci.nsICommandLine.STATE_REMOTE_AUTO) {
      cmdLine.preventDefault = true;
    }
  },

  helpInfo: "  --oop        Use out-of-process model in B2G\n",
  classID: Components.ID("{e30b0e13-2d12-4cb0-bc4c-4e617a1bf76e}"),
  QueryInterface: ChromeUtils.generateQI([Ci.nsICommandLineHandler]),
};

this.NSGetFactory = ComponentUtils.generateNSGetFactory([
  oopCommandlineHandler,
]);
