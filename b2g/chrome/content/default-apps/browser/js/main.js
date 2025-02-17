/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

let tabs = [];
let activatedTab = null;
let commands = ["action-back", "action-forward", "action-go", "action-close"];

function log(msg) {
  console.log(`Default browser app: ${msg}`);
}

class BrowserTab {
  constructor(document, url, browserElement) {
    this.parentDocument = document;
    this.webview = this.parentDocument.createElement("web-view");
    this.webview.openWindowInfo = null;
    if (browserElement) {
      this.webview.browserElement = browserElement;
    } else {
      this.webview.setAttribute("id", "browser");
      this.webview.setAttribute("src", url);
    }
    this.parentDocument.body.append(this.webview);
    this.parentDocument
      .getElementById("action-go")
      .toggleAttribute("disabled", false);
    this.parentDocument
      .getElementById("action-close")
      .toggleAttribute("disabled", false);
    this.show();
  }

  show() {
    if (activatedTab) {
      activatedTab.hide();
    }
    this.webview.style.display = "block";

    // Binding events
    this.webview.addEventListener("locationchange", this.updateActionsUI);
    this.webview.addEventListener("openwindow", this.openWindow);
    this.webview.addEventListener("iconchange", this.setupIcon);
    this.webview.addEventListener("titlechange", this.updateTitle);
    this.webview.addEventListener("opensearch", this.updateSearch);

    this.parentDocument.getElementById("url").value = this.webview.src;
    this.parentDocument
      .getElementById("action-back")
      .toggleAttribute("disabled", !this.webview.canGoBack);
    this.parentDocument
      .getElementById("action-forward")
      .toggleAttribute("disabled", !this.webview.canGoForward);
    activatedTab = this;
    this.setupIcon({});
    this.updateTitle({});
    this.updateSearch({});
  }

  hide() {
    if (activatedTab != this) {
      return;
    }
    this.webview.style.display = "none";
    this.webview.removeEventListener("locationchange", this.updateActionsUI);
    this.webview.removeEventListener("openwindow", this.openWindow);
    this.webview.removeEventListener("iconchange", this.setupIcon);

    this.parentDocument.getElementById("url").value = "";
    this.parentDocument
      .getElementById("action-back")
      .toggleAttribute("disabled", true);
    this.parentDocument
      .getElementById("action-forward")
      .toggleAttribute("disabled", true);
    this.setupIcon({});
    this.updateTitle({});
    this.updateSearch({});
    activatedTab = null;
  }

  openWindow(aEvent) {
    log("openWindow");
    tabs.push(
      new BrowserTab(
        activatedTab.parentDocument,
        null,
        aEvent.detail.frameElement
      )
    );
  }

  setupIcon(aEvent) {
    log("setupIcon");
    let href = aEvent?.detail?.href;
    if (activatedTab) {
      activatedTab.parentDocument.querySelector(
        "#tabs > .icon"
      ).style.display = href ? "inline" : "none";
      activatedTab.parentDocument.querySelector("#tabs > .icon").src = href;
    }
  }

  updateTitle(aEvent) {
    let title = aEvent?.detail?.title;
    log("updateTitle " + title);
    if (activatedTab) {
      activatedTab.parentDocument.querySelector(
        "#tabs > .title"
      ).innerText = title;
    }
  }

  updateSearch(aEvent) {
    let title = aEvent?.detail?.title;
    let href = aEvent?.detail?.href;
    log(`updateSearch ${title}:${href}`);
    if (activatedTab) {
      activatedTab.parentDocument.querySelector("#tabs > .search").innerText =
        title + " " + href;
    }
  }

  updateActionsUI(aEvent) {
    log(
      "updateActionsUI " +
        aEvent.detail.url +
        " " +
        aEvent.detail.canGoBack +
        " " +
        aEvent.detail.canGoForward
    );
    activatedTab.parentDocument
      .getElementById("action-back")
      .toggleAttribute("disabled", !aEvent.detail.canGoBack);
    activatedTab.parentDocument
      .getElementById("action-forward")
      .toggleAttribute("disabled", !aEvent.detail.canGoForward);
    activatedTab.parentDocument.getElementById("url").value = aEvent.detail.url;
  }

  go() {
    log("go " + this.parentDocument.getElementById("url").value);
    this.webview.src = this.parentDocument.getElementById("url").value;
  }

  remove() {
    this.webview.remove();
    this.parentDocument
      .getElementById("action-go")
      .toggleAttribute("disabled", !tabs.length);
    this.parentDocument
      .getElementById("action-close")
      .toggleAttribute("disabled", !tabs.length);
  }

  goForward() {
    this.setupIcon({});
    this.webview.goForward();
  }

  goBack() {
    this.setupIcon({});
    this.webview.goBack();
  }
}

document.addEventListener(
  "DOMContentLoaded",
  () => {
    const startURL =
      "https://www.w3schools.com/jsref/tryit.asp?filename=tryjsref_win_open";
    this.go = function() {
      activatedTab.go();
    };

    this.close = function() {
      let closedTab = activatedTab;
      const index = tabs.indexOf(closedTab);
      if (index > -1) {
        tabs.splice(index, 1);
        closedTab.hide();
        if (tabs.length) {
          tabs[tabs.length - 1].show();
        }
        setTimeout(() => {
          closedTab.remove();
        }, 0);
      }
    };

    this.forward = function() {
      activatedTab.goForward();
    };

    this.back = function() {
      activatedTab.goBack();
    };

    let openBtn = document.getElementById("action-open");
    openBtn.addEventListener("click", () => {
      tabs.push(new BrowserTab(document, startURL, null));
    });

    // Binding Actions
    commands.forEach(id => {
      let btn = document.getElementById(id);
      btn.toggleAttribute("disabled", true);
      btn.addEventListener("click", this[btn.dataset.cmd]);
    });
  },
  { once: true }
);
