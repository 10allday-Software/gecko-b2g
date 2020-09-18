/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain at http://mozilla.org/MPL/2.0/. */

[JSImplementation="@mozilla.org/permissions-manager;1",
 Pref="dom.permissions.manager.enabled",
 Exposed=Window]
interface PermissionsManager
{
  DOMString get(DOMString permission, DOMString origin);

  void set(DOMString permission, DOMString value, DOMString origin);

  boolean isExplicit(DOMString permission, DOMString origin);

  void remove(DOMString permission, DOMString origin);
};
