/*
 * Copyright 2013 Ian Pilcher <arequipeno@gmail.com>
 *
 * This program is free software.  You can redistribute it or modify it under
 * the terms of version 2 of the GNU General Public License (GPL), as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY -- without even the implied warranty of MERCHANTIBILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the text of the GPL for more details.
 *
 * Version 2 of the GNU General Public License is available at:
 *
 *   http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "freecusd.h"

void fcd_update_alert(enum fcd_alert new, enum fcd_alert *status)
{
	if (new == FCD_ALERT_SET) {
		switch (*status)
		{
			case FCD_ALERT_SET:
				/*
				 * We set the alert last time through, but the
				 * main thread hasn't noticed it yet.  We still
				 * want to set it.
				 */
				break;

			case FCD_ALERT_CLR:
				/*
				 * We cleared the alert last time through, but
				 * the main thread hasn't noticed it yet.  The
				 * fact that we cleared it last time means that
				 * it must currently be set, which is what we
				 * want.
				 */
				*status = FCD_ALERT_SET_ACK;
				break;

			case FCD_ALERT_SET_ACK:
				/*
				 * Alert is currently set, which is what we
				 * want.
				 */
				break;

			case FCD_ALERT_CLR_ACK:
				/* Alert is currently cleared; set it. */
				*status = FCD_ALERT_SET;
				break;
		}
	}
	else if (new == FCD_ALERT_CLR) {
		switch (*status)
		{
			case FCD_ALERT_SET:
				/*
				 * We set the alert the last time through, but
				 * the main thread hasn't noticed it yet.  The
				 * fact that we set it last time means that it
				 * must currently be cleared, which is what we
				 * want.
				 */
				*status = FCD_ALERT_CLR_ACK;
				break;

			case FCD_ALERT_CLR:
				/*
				 * We cleared the alert last time through, but
				 * the main thread hasn't noticed it yet.  We
				 * still want to clear it.
				 */
				break;

			case FCD_ALERT_SET_ACK:
				/* Alert is currently set; clear it. */
				*status = FCD_ALERT_CLR;
				break;

			case FCD_ALERT_CLR_ACK:
				/*
				 * Alert is currently cleared, which is what we
				 * want.
				 */
				break;
		}
	}
	else {
		FCD_ABORT("%s\n", strerror(EINVAL));
	}
}
