/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

[Exposed=Window, Pref="dom.telephony.enabled",
// CheckAnyPermissions="telephony",
// AvailableIn="CertifiedApps",
]
interface USSDSession {
  [Throws]
  constructor(unsigned long serviceId);

  [NewObject]
  Promise<void> send(DOMString ussd);

  [NewObject]
  Promise<void> cancel();
};
