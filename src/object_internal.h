/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_OBJECT_INTERNAL_H
#define UIB_OBJECT_INTERNAL_H

struct uib_cq;
struct uib_pd;

int uib_cq_get(struct uib_cq *cq);
void uib_cq_put(struct uib_cq *cq);
int uib_pd_get(struct uib_pd *pd);
void uib_pd_put(struct uib_pd *pd);

#endif
